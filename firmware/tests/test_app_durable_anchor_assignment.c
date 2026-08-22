#include "app_durable_state.h"
#include "discovery_assignment.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_DEVICE_A UINT64_C(0x1020304050607080)
#define TEST_GATEWAY_A UINT64_C(0x8877665544332211)
#define TEST_GATEWAY_B UINT64_C(0x8877665544332212)
#define TEST_SLOT_COUNT 6u

#define TEST_MAGIC_OFFSET 0u
#define TEST_SCHEMA_OFFSET 4u
#define TEST_TOTAL_SIZE_OFFSET 6u
#define TEST_TYPE_OFFSET 8u
#define TEST_ROLE_OFFSET 12u
#define TEST_DEVICE_ID_OFFSET 16u
#define TEST_PAYLOAD_SIZE_OFFSET 24u
#define TEST_CRC_OFFSET 26u
#define TEST_PAYLOAD_OFFSET APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define TEST_RETIRED_EPOCHS_OFFSET 120u
#define TEST_RETIRED_EPOCH_COUNT_OFFSET 191u
#define TEST_PENDING_VALID_OFFSET 196u
#define TEST_LEGACY_V8_RECORD_SIZE 184u
#define TEST_LEGACY_V8_MAGIC_OFFSET 160u
#define TEST_LEGACY_V8_MAGIC UINT32_C(0x44415338)

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
    unsigned int erase_calls;
    int mount_error;
    int next_read_error;
    int next_write_error;
    int next_erase_error;
    size_t next_torn_write_bytes;
    bool acknowledge_without_commit;
    bool fail_read_after_write;
    bool fail_read_after_erase;
};

static void test_put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void test_put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void test_put_u64(uint8_t *dst, uint64_t value)
{
    for (uint8_t index = 0u; index < sizeof(value); index++) {
        dst[index] = (uint8_t)(value >> (8u * index));
    }
}

static uint16_t test_get_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t test_get_u32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t test_get_u64(const uint8_t *src)
{
    uint64_t value = 0u;

    for (uint8_t index = 0u; index < sizeof(value); index++) {
        value |= (uint64_t)src[index] << (8u * index);
    }
    return value;
}

static uint16_t test_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = UINT16_C(0xffff);

    for (size_t index = 0u; index < len; index++) {
        crc ^= (uint16_t)data[index] << 8;
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            crc = (crc & UINT16_C(0x8000)) != 0u
                      ? (uint16_t)((crc << 1) ^ UINT16_C(0x1021))
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void test_slot_recompute_crc(struct fake_slot *slot)
{
    assert(slot != NULL);
    assert(slot->present);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], 0u);
    test_put_u16(&slot->data[TEST_CRC_OFFSET],
                 test_crc16(slot->data, slot->len));
}

static struct fake_slot *fake_find_slot(struct fake_store *store,
                                        uint16_t id,
                                        bool allocate)
{
    struct fake_slot *free_slot = NULL;

    assert(store != NULL);
    for (size_t index = 0u; index < TEST_SLOT_COUNT; index++) {
        if (store->slots[index].present && store->slots[index].id == id) {
            return &store->slots[index];
        }
        if (!store->slots[index].present && free_slot == NULL) {
            free_slot = &store->slots[index];
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
    if (store->acknowledge_without_commit) {
        store->acknowledge_without_commit = false;
        return (ssize_t)len;
    }

    slot = fake_find_slot(store, id, true);
    assert(slot != NULL);
    assert(len <= sizeof(slot->data));
    if (store->next_torn_write_bytes != 0u) {
        size_t copied = store->next_torn_write_bytes;

        if (copied > len) {
            copied = len;
        }
        memset(slot->data, 0, len);
        memcpy(slot->data, data, copied);
        slot->len = len;
        slot->present = true;
        store->next_torn_write_bytes = 0u;
    } else if (slot->present && slot->len == len &&
               memcmp(slot->data, data, len) == 0) {
        if (store->fail_read_after_write) {
            store->next_read_error = -ETIMEDOUT;
            store->fail_read_after_write = false;
        }
        return 0;
    } else {
        memcpy(slot->data, data, len);
        slot->len = len;
        slot->present = true;
    }
    if (store->fail_read_after_write) {
        store->next_read_error = -ETIMEDOUT;
        store->fail_read_after_write = false;
    }
    return (ssize_t)len;
}

static int fake_erase(void *context, uint16_t id)
{
    struct fake_store *store = context;
    struct fake_slot *slot;

    store->erase_calls++;
    if (store->next_erase_error != 0) {
        int error = store->next_erase_error;

        store->next_erase_error = 0;
        return error;
    }
    slot = fake_find_slot(store, id, false);
    if (slot == NULL) {
        return -ENOENT;
    }
    memset(slot->data, 0, sizeof(slot->data));
    slot->len = 0u;
    slot->present = false;
    if (store->fail_read_after_erase) {
        store->next_read_error = -ETIMEDOUT;
        store->fail_read_after_erase = false;
    }
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
                          enum app_durable_state_role role)
{
    install_backend(store, role);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_ready());
    assert(app_durable_state_begin_boot() == 0);
}

static struct fake_slot *assignment_slot(struct fake_store *store)
{
    for (size_t index = 0u; index < TEST_SLOT_COUNT; index++) {
        struct fake_slot *slot = &store->slots[index];

        if (!slot->present || slot->len <= TEST_TYPE_OFFSET + 1u) {
            continue;
        }
        if (test_get_u16(&slot->data[TEST_TYPE_OFFSET]) ==
            APP_DURABLE_STATE_ANCHOR_ASSIGNMENT) {
            return slot;
        }
    }
    assert(false);
    return NULL;
}

static void fill_commitment(
    struct discovery_assignment_table_commitment *commitment,
    uint8_t seed)
{
    assert(commitment != NULL);
    for (size_t index = 0u; index < sizeof(commitment->bytes); index++) {
        commitment->bytes[index] = (uint8_t)(seed + index);
    }
}

static struct app_durable_state_anchor_assignment pending_assignment(void)
{
    struct app_durable_state_anchor_assignment assignment = {0};

    fill_commitment(&assignment.pending_table_commitment, 0x40u);
    assignment.pending_epoch = 11u;
    assignment.pending_table_command_seq = 101u;
    assignment.table_packet_seq = 37u;
    assignment.response_spread_ms = 1000u;
    assignment.ordered_epoch_valid = 1u;
    assignment.ack_pending = 1u;
    assignment.pending_slot = 2u;
    assignment.pending_slot_count = 4u;
    assignment.pending_response_lane = 1u;
    assignment.pending_response_lane_count = 3u;
    assignment.pending_valid = 1u;
    return assignment;
}

static struct app_durable_state_anchor_assignment promoted_assignment(void)
{
    struct app_durable_state_anchor_assignment assignment =
        pending_assignment();

    assignment.table_commitment = assignment.pending_table_commitment;
    assignment.epoch = assignment.pending_epoch;
    assignment.table_command_seq = assignment.pending_table_command_seq;
    assignment.slot = assignment.pending_slot;
    assignment.slot_count = assignment.pending_slot_count;
    assignment.provisioned = 1u;
    memset(&assignment.pending_table_commitment,
           0,
           sizeof(assignment.pending_table_commitment));
    assignment.pending_epoch = 0u;
    assignment.pending_table_command_seq = 0u;
    assignment.table_packet_seq = 0u;
    assignment.response_spread_ms = 0u;
    assignment.ack_pending = 0u;
    assignment.pending_slot = 0u;
    assignment.pending_slot_count = 0u;
    assignment.pending_response_lane = 0u;
    assignment.pending_response_lane_count = 0u;
    assignment.pending_valid = 0u;
    return assignment;
}

static bool assignment_equal(
    const struct app_durable_state_anchor_assignment *left,
    const struct app_durable_state_anchor_assignment *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->table_commitment.bytes,
                  right->table_commitment.bytes,
                  sizeof(left->table_commitment.bytes)) == 0 &&
           memcmp(left->pending_table_commitment.bytes,
                  right->pending_table_commitment.bytes,
                  sizeof(left->pending_table_commitment.bytes)) == 0 &&
           left->epoch == right->epoch &&
           left->table_command_seq == right->table_command_seq &&
           left->pending_epoch == right->pending_epoch &&
           left->pending_table_command_seq ==
               right->pending_table_command_seq &&
           memcmp(left->retired_epochs,
                  right->retired_epochs,
                  sizeof(left->retired_epochs)) == 0 &&
           left->table_packet_seq == right->table_packet_seq &&
           left->response_spread_ms == right->response_spread_ms &&
           left->slot == right->slot &&
           left->slot_count == right->slot_count &&
           left->provisioned == right->provisioned &&
           left->retired_epoch_count == right->retired_epoch_count &&
           left->ordered_epoch_valid == right->ordered_epoch_valid &&
           left->ack_pending == right->ack_pending &&
           left->pending_slot == right->pending_slot &&
           left->pending_slot_count == right->pending_slot_count &&
           left->pending_response_lane == right->pending_response_lane &&
           left->pending_response_lane_count ==
               right->pending_response_lane_count &&
           left->pending_valid == right->pending_valid;
}

static bool assignment_is_zero(
    const struct app_durable_state_anchor_assignment *assignment)
{
    const struct app_durable_state_anchor_assignment zero = {0};

    return assignment_equal(assignment, &zero);
}

static void test_missing_and_canonical_round_trip(void)
{
    struct fake_store store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment restored;
    struct fake_slot *slot;
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    memset(&restored, 0xa5, sizeof(restored));
    writes = store.write_calls;
    store.next_read_error = -ETIMEDOUT;
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -ETIMEDOUT);
    assert(assignment_is_zero(&restored));
    assert(store.write_calls == writes);

    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 0);
    assert(assignment_is_zero(&restored));
    assert(store.write_calls == writes);

    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    assert(store.write_calls == writes + 1u);
    slot = assignment_slot(&store);
    assert(slot->len == 200u);
    assert(test_get_u32(&slot->data[TEST_MAGIC_OFFSET]) != 0u);
    assert(test_get_u16(&slot->data[TEST_SCHEMA_OFFSET]) ==
           APP_DURABLE_STATE_ENVELOPE_VERSION);
    assert(test_get_u16(&slot->data[TEST_TOTAL_SIZE_OFFSET]) == slot->len);
    assert(test_get_u16(&slot->data[TEST_TYPE_OFFSET]) ==
           APP_DURABLE_STATE_ANCHOR_ASSIGNMENT);
    assert(test_get_u16(&slot->data[TEST_PAYLOAD_SIZE_OFFSET]) == 168u);
    assert(test_get_u64(&slot->data[TEST_PAYLOAD_OFFSET]) == TEST_GATEWAY_A);
    assert(slot->data[slot->len - 3u] == pending.pending_response_lane);
    assert(slot->data[slot->len - 2u] ==
           pending.pending_response_lane_count);
    assert(slot->data[slot->len - 1u] == 0u);

    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));
}

static void test_logically_invalid_records_are_rejected_without_writes(void)
{
    struct fake_store store = {0};
    struct app_durable_state_anchor_assignment assignment = {0};
    struct app_durable_state_anchor_assignment restored;
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    writes = store.write_calls;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, NULL) == -EINVAL);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, NULL) == -EINVAL);
    assert(app_durable_state_save_anchor_assignment(
               0u, &assignment) == -EINVAL);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(0u, &restored) ==
           -EINVAL);
    assert(app_durable_state_delete_anchor_assignment(0u) == -EINVAL);
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);

    assignment = pending_assignment();
    assignment.table_packet_seq = 0u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment = pending_assignment();
    assignment.response_spread_ms = 0u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment = pending_assignment();
    assignment.pending_slot = assignment.pending_slot_count;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment = pending_assignment();
    assignment.ack_pending = 2u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment = pending_assignment();
    assignment.retired_epochs[0] = 7u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment.retired_epoch_count = 1u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);

    assignment = promoted_assignment();
    assignment.slot = assignment.slot_count;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment = promoted_assignment();
    assignment.pending_epoch = assignment.epoch;
    assignment.pending_table_command_seq = 202u;
    fill_commitment(&assignment.pending_table_commitment, 0x80u);
    assignment.pending_slot = 1u;
    assignment.pending_slot_count = assignment.slot_count;
    assignment.pending_valid = 1u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);

    assignment = promoted_assignment();
    assignment.pending_epoch = assignment.epoch + 2u;
    assignment.pending_table_command_seq = 202u;
    fill_commitment(&assignment.pending_table_commitment, 0x80u);
    assignment.pending_slot_count = assignment.slot_count;
    assignment.pending_valid = 1u;
    assignment.retired_epochs[0] = assignment.epoch;
    assignment.retired_epoch_count = 1u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assignment.retired_epochs[0] = assignment.epoch + 1u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &assignment) == -EINVAL);
    assert(store.write_calls == writes);
}

static void test_corruption_and_all_bindings_fail_closed(void)
{
    struct fake_store store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment restored;
    struct fake_slot *slot;
    uint8_t canonical[APP_DURABLE_STATE_MAX_RECORD_SIZE];
    size_t canonical_len;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    slot = assignment_slot(&store);
    canonical_len = slot->len;
    memcpy(canonical, slot->data, canonical_len);

    slot->data[TEST_PAYLOAD_OFFSET + sizeof(uint64_t)] ^= 1u;
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EBADMSG);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    slot->data[TEST_ROLE_OFFSET] = APP_DURABLE_STATE_ROLE_GATEWAY;
    test_slot_recompute_crc(slot);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EACCES);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    test_put_u64(&slot->data[TEST_DEVICE_ID_OFFSET], TEST_DEVICE_A ^ 1u);
    test_slot_recompute_crc(slot);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EACCES);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    slot->data[slot->len - 1u] = 1u;
    test_slot_recompute_crc(slot);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EPROTO);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    slot->data[TEST_PENDING_VALID_OFFSET] = 2u;
    test_slot_recompute_crc(slot);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EPROTO);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    test_put_u32(&slot->data[TEST_RETIRED_EPOCHS_OFFSET], 7u);
    slot->data[TEST_RETIRED_EPOCH_COUNT_OFFSET] = 1u;
    test_slot_recompute_crc(slot);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EPROTO);
    assert(assignment_is_zero(&restored));

    memcpy(slot->data, canonical, canonical_len);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_B, &restored) == -EACCES);
    assert(assignment_is_zero(&restored));

    memset(slot->data, 0, sizeof(slot->data));
    slot->len = TEST_LEGACY_V8_RECORD_SIZE;
    test_put_u32(&slot->data[TEST_LEGACY_V8_MAGIC_OFFSET],
                 TEST_LEGACY_V8_MAGIC);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) < 0);
    assert(assignment_is_zero(&restored));
}

static void test_non_anchor_roles_cannot_access_assignment_record(void)
{
    struct fake_store clicker_store = {0};
    struct fake_store gateway_store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment restored;

    install_store(&clicker_store, APP_DURABLE_STATE_ROLE_CLICKER);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == -ENOTSUP);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -ENOTSUP);
    assert(assignment_is_zero(&restored));
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) ==
           -ENOTSUP);

    install_store(&gateway_store, APP_DURABLE_STATE_ROLE_GATEWAY);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == -ENOTSUP);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -ENOTSUP);
    assert(assignment_is_zero(&restored));
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) ==
           -ENOTSUP);
}

static void test_save_crash_cuts_keep_outcomes_explicit(void)
{
    struct fake_store store = {0};
    struct fake_store torn_store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment promoted =
        promoted_assignment();
    struct app_durable_state_anchor_assignment restored;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    store.acknowledge_without_commit = true;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == -EIO);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 0);
    assert(assignment_is_zero(&restored));

    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    store.next_write_error = -EIO;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &promoted) == -EIO);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));

    store.acknowledge_without_commit = true;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &promoted) == -EIO);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));

    store.fail_read_after_write = true;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &promoted) == -ETIMEDOUT);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &promoted));
    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &promoted));

    install_store(&torn_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    torn_store.next_torn_write_bytes = 100u;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &promoted) == -EBADMSG);
    memset(&restored, 0xa5, sizeof(restored));
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == -EBADMSG);
    assert(assignment_is_zero(&restored));
}

static void test_delete_is_checked_and_idempotent(void)
{
    struct fake_store store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment restored;
    struct fake_slot *slot;
    uint8_t canonical[APP_DURABLE_STATE_MAX_RECORD_SIZE];
    size_t canonical_len;
    unsigned int erases;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    erases = store.erase_calls;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) == 0);
    assert(store.erase_calls == erases);
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    slot = assignment_slot(&store);
    canonical_len = slot->len;
    memcpy(canonical, slot->data, canonical_len);

    erases = store.erase_calls;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_B) ==
           -EACCES);
    assert(store.erase_calls == erases);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));

    slot->data[TEST_PAYLOAD_OFFSET + sizeof(uint64_t)] ^= 1u;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) ==
           -EBADMSG);
    assert(store.erase_calls == erases);
    memcpy(slot->data, canonical, canonical_len);

    store.next_erase_error = -EIO;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) ==
           -EIO);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));

    store.fail_read_after_erase = true;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) ==
           -ETIMEDOUT);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 0);
    assert(assignment_is_zero(&restored));

    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) == 0);
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 0);
    assert(assignment_is_zero(&restored));
    erases = store.erase_calls;
    assert(app_durable_state_delete_anchor_assignment(TEST_GATEWAY_A) == 0);
    assert(store.erase_calls == erases);
}

static void test_pending_promotion_costs_exactly_two_writes(void)
{
    struct fake_store store = {0};
    struct app_durable_state_anchor_assignment pending = pending_assignment();
    struct app_durable_state_anchor_assignment promoted =
        promoted_assignment();
    struct app_durable_state_anchor_assignment restored;
    unsigned int initial_writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_ANCHOR);
    initial_writes = store.write_calls;
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &pending) == 0);
    assert(store.write_calls == initial_writes + 1u);

    for (size_t replay = 0u; replay < 8u; replay++) {
        assert(app_durable_state_save_anchor_assignment(
                   TEST_GATEWAY_A, &pending) == 0);
        assert(store.write_calls == initial_writes + 1u);
    }
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &pending));
    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 0);
    assert(store.write_calls == initial_writes + 1u);

    assert(app_durable_state_save_anchor_assignment(
               TEST_GATEWAY_A, &promoted) == 0);
    assert(store.write_calls == initial_writes + 2u);
    for (size_t replay = 0u; replay < 8u; replay++) {
        assert(app_durable_state_save_anchor_assignment(
                   TEST_GATEWAY_A, &promoted) == 0);
        assert(store.write_calls == initial_writes + 2u);
    }
    assert(app_durable_state_restore_anchor_assignment(
               TEST_GATEWAY_A, &restored) == 1);
    assert(assignment_equal(&restored, &promoted));
}

int main(void)
{
    test_missing_and_canonical_round_trip();
    test_logically_invalid_records_are_rejected_without_writes();
    test_corruption_and_all_bindings_fail_closed();
    test_non_anchor_roles_cannot_access_assignment_record();
    test_save_crash_cuts_keep_outcomes_explicit();
    test_delete_is_checked_and_idempotent();
    test_pending_promotion_costs_exactly_two_writes();
    return 0;
}
