#include "app_durable_state.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_DEVICE_A UINT64_C(0x1020304050607080)
#define TEST_DEVICE_B UINT64_C(0x1020304050607081)
#define TEST_GATEWAY_A UINT64_C(0x8877665544332211)
#define TEST_GATEWAY_B UINT64_C(0x8877665544332212)
#define TEST_BOOT_NVS_ID UINT16_C(0x0300)
#define TEST_GATEWAY_ASSIGNMENT_NVS_ID UINT16_C(0x0302)
#define TEST_SLOT_COUNT 4u

#define TEST_CRC_OFFSET 26u
#define TEST_GATEWAY_ID_OFFSET APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define TEST_GATEWAY_SEQUENCE_OFFSET \
    (TEST_GATEWAY_ID_OFFSET + sizeof(uint64_t) + sizeof(uint32_t))

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
    bool drop_next_write;
    bool fail_next_read_after_write;
    bool fail_next_read_after_erase;
    int next_read_error;
};

static const uint64_t sparse_ids[] = {
    UINT64_C(0x1001),
    UINT64_C(0x2002),
    UINT64_C(0x4004),
};
static const uint8_t sparse_slots[] = {0u, 2u, 4u};
static const struct discovery_assignment_table_commitment commitment = {
    .bytes = {
        0x65u, 0xabu, 0xcdu, 0xefu, 0x01u, 0x23u, 0x45u, 0x67u,
        0x89u, 0xabu, 0xcdu, 0xefu, 0xfeu, 0xdcu, 0xbau, 0x98u,
        0x76u, 0x54u, 0x32u, 0x10u, 0x5au, 0xa5u, 0xc3u, 0x3cu,
        0x96u, 0x69u, 0xf0u, 0x0fu, 0x55u, 0xaau, 0x12u, 0x21u,
    },
};

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

static void test_put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void test_put_u32(uint8_t *dst, uint32_t value)
{
    for (uint8_t index = 0u; index < sizeof(value); index++) {
        dst[index] = (uint8_t)(value >> (8u * index));
    }
}

static void test_put_u64(uint8_t *dst, uint64_t value)
{
    for (uint8_t index = 0u; index < sizeof(value); index++) {
        dst[index] = (uint8_t)(value >> (8u * index));
    }
}

static struct fake_slot *fake_find_slot(struct fake_store *store,
                                        uint16_t id,
                                        bool allocate)
{
    struct fake_slot *available = NULL;

    for (size_t index = 0u; index < TEST_SLOT_COUNT; index++) {
        if (store->slots[index].present && store->slots[index].id == id) {
            return &store->slots[index];
        }
        if (!store->slots[index].present && available == NULL) {
            available = &store->slots[index];
        }
    }
    if (!allocate || available == NULL) {
        return NULL;
    }
    available->id = id;
    return available;
}

static int fake_mount(void *context)
{
    struct fake_store *store = context;

    store->mount_calls++;
    return 0;
}

static ssize_t fake_read(void *context,
                         uint16_t id,
                         void *data,
                         size_t len)
{
    struct fake_store *store = context;
    struct fake_slot *slot = fake_find_slot(store, id, false);

    store->read_calls++;
    if (store->next_read_error != 0) {
        int error = store->next_read_error;

        store->next_read_error = 0;
        return error;
    }
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
    if (store->drop_next_write) {
        store->drop_next_write = false;
        return (ssize_t)len;
    }
    slot = fake_find_slot(store, id, true);
    assert(slot != NULL);
    assert(len <= sizeof(slot->data));
    if (slot->present && slot->len == len &&
        memcmp(slot->data, data, len) == 0) {
        return 0;
    }
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->present = true;
    if (store->fail_next_read_after_write) {
        store->fail_next_read_after_write = false;
        store->next_read_error = -ETIMEDOUT;
    }
    return (ssize_t)len;
}

static int fake_erase(void *context, uint16_t id)
{
    struct fake_store *store = context;
    struct fake_slot *slot = fake_find_slot(store, id, false);

    store->erase_calls++;
    if (slot == NULL) {
        return -ENOENT;
    }
    memset(slot, 0, sizeof(*slot));
    if (store->fail_next_read_after_erase) {
        store->fail_next_read_after_erase = false;
        store->next_read_error = -ETIMEDOUT;
    }
    return 0;
}

static void install_store(struct fake_store *store, uint64_t device_id)
{
    const struct app_durable_state_test_backend backend = {
        .context = store,
        .mount = fake_mount,
        .read = fake_read,
        .write = fake_write,
        .erase = fake_erase,
    };

    app_durable_state_test_reset();
    assert(app_durable_state_test_install_backend(
               &backend, APP_DURABLE_STATE_ROLE_GATEWAY) == 0);
    assert(app_durable_state_init(device_id) == 0);
    assert(app_durable_state_begin_boot() == 0);
}

static struct app_durable_state_gateway_assignment_identity test_identity(
    uint32_t variant)
{
    return (struct app_durable_state_gateway_assignment_identity) {
        .correlation_id = UINT32_C(0x99aabbc0) + variant,
        .gateway_sequence = UINT32_C(0x55667780) + variant,
        .host_session_id = UINT32_C(0x99aabbc0) + variant,
        .gateway_epoch = (uint16_t)(UINT16_C(0x4320) + variant),
        .host_seq = (uint16_t)(UINT16_C(0x1200) + variant),
    };
}

static struct gateway_membership_snapshot test_snapshot(
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    struct gateway_membership_roster roster = {0};
    struct gateway_membership_publication publication = {0};
    struct gateway_membership_snapshot snapshot;

    assert(identity != NULL);
    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               discovery_assignment_membership_epoch(
                   identity->gateway_sequence),
               sparse_ids,
               sparse_slots,
               sizeof(sparse_ids) / sizeof(sparse_ids[0])) == PROTO_OK);
    publication.claimed_node_ids[0] = sparse_ids[0];
    publication.claimed_node_ids[2] = sparse_ids[1];
    publication.claimed_node_ids[4] = sparse_ids[2];
    publication.host_command = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = UINT64_C(0xabc),
        .dst_id = TEST_GATEWAY_A,
        .session_id = identity->host_session_id,
        .seq = identity->host_seq,
        .ttl = 3u,
        .payload_len = PROTO_TLV_U16_ENCODED_LEN,
        .message_age_ms = 7u,
    };
    publication.committed_mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 2) | (UINT64_C(1) << 4);
    publication.acknowledged_mask =
        (UINT64_C(1) << 0) | (UINT64_C(1) << 4);
    publication.command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication.event_gateway_epoch = identity->gateway_epoch;
    publication.duplicate_count = 2u;
    publication.claimed_count = 3u;
    publication.claimed_slot_span = 5u;
    publication.table_round = 1u;
    publication.publish_pending = true;
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               identity->gateway_sequence,
               UINT32_C(0x92345678),
               &commitment,
               &publication,
               &snapshot) == PROTO_OK);
    return snapshot;
}

static struct gateway_command_event terminal_event(
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    return (struct gateway_command_event) {
        .schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION,
        .record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        .flags = GATEWAY_COMMAND_EVENT_FLAG_TERMINAL,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = identity->gateway_epoch,
        .correlation_id = identity->correlation_id,
        .gateway_sequence = identity->gateway_sequence,
        .host_session_id = identity->host_session_id,
        .host_seq = identity->host_seq,
        .event_seq = UINT32_C(0x778899aa),
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
}

static void assert_sparse_roster(const struct gateway_membership_snapshot *snapshot)
{
    struct gateway_membership_roster roster = {0};

    assert(gateway_membership_restore_snapshot(&roster, snapshot) == PROTO_OK);
    assert(roster.node_count == 3u);
    assert(roster.slot_span == 5u);
    assert(roster.node_ids[0] == sparse_ids[0]);
    assert(roster.node_ids[1] == 0u);
    assert(roster.node_ids[2] == sparse_ids[1]);
    assert(roster.node_ids[3] == 0u);
    assert(roster.node_ids[4] == sparse_ids[2]);
}

static void test_commit_retire_reboot_and_successor_admission(void)
{
    struct fake_store store = {0};
    struct app_durable_state_gateway_assignment_identity identity =
        test_identity(1u);
    struct app_durable_state_gateway_assignment_identity successor_identity =
        test_identity(8u);
    struct gateway_membership_snapshot snapshot = test_snapshot(&identity);
    struct gateway_membership_snapshot successor_snapshot =
        test_snapshot(&successor_identity);
    struct gateway_membership_snapshot restored = {0};
    struct app_durable_state_gateway_assignment_identity restored_identity = {0};
    struct app_durable_receipt receipt = {0};
    struct app_durable_receipt restored_receipt = {0};
    struct app_durable_receipt retired_receipt = {0};
    struct app_durable_receipt wrong_receipt;
    struct gateway_command_event terminal = terminal_event(&identity);
    struct gateway_command_event wrong_terminal;
    bool replay_debt = false;
    unsigned int writes_before_commit;
    unsigned int writes_before_retire;

    install_store(&store, TEST_DEVICE_A);
    writes_before_commit = store.write_calls;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    assert(store.write_calls == writes_before_commit + 1u);
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &restored_receipt) == 0);
    assert(store.write_calls == writes_before_commit + 1u);
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &successor_snapshot,
               &successor_identity,
               &restored_receipt) == -EBUSY);
    assert(store.write_calls == writes_before_commit + 1u);

    install_store(&store, TEST_DEVICE_A);
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &restored_receipt) == 1);
    assert(replay_debt);
    assert(gateway_membership_snapshot_semantically_equal(&snapshot,
                                                           &restored));
    assert(memcmp(&identity, &restored_identity, sizeof(identity)) == 0);
    assert_sparse_roster(&restored);

    writes_before_retire = store.write_calls;
    wrong_terminal = terminal;
    wrong_terminal.stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored_receipt,
               &wrong_terminal,
               &retired_receipt) == -ESTALE);
    assert(store.write_calls == writes_before_retire);
    store.fail_next_read_after_write = true;
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored_receipt,
               &terminal,
               &retired_receipt) == -ETIMEDOUT);
    assert(store.write_calls == writes_before_retire + 1u);
    assert(retired_receipt.opaque[0] == 0u && retired_receipt.opaque[1] == 0u);
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &(struct app_durable_receipt){0},
               &terminal,
               &retired_receipt) == -ESTALE);
    wrong_receipt = restored_receipt;
    wrong_receipt.opaque[0] ^= UINT64_C(1);
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &wrong_receipt,
               &terminal,
               &retired_receipt) == -ESTALE);
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored_receipt,
               &terminal,
               &retired_receipt) == 0);
    assert(store.write_calls == writes_before_retire + 1u);

    install_store(&store, TEST_DEVICE_A);
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &restored_receipt) == 1);
    assert(!replay_debt);
    assert_sparse_roster(&restored);
    assert(gateway_membership_snapshot_get_publication(
               &restored,
               &(struct gateway_membership_publication){0}) ==
           PROTO_ERR_NOT_FOUND);

    identity = test_identity(2u);
    snapshot = test_snapshot(&identity);
    writes_before_commit = store.write_calls;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    assert(store.write_calls == writes_before_commit + 1u);
}

static void test_save_readback_retry_adopts_exact_commit(void)
{
    struct fake_store store = {0};
    struct app_durable_state_gateway_assignment_identity identity =
        test_identity(4u);
    struct gateway_membership_snapshot snapshot = test_snapshot(&identity);
    struct app_durable_receipt receipt = {0};
    unsigned int writes_before;

    install_store(&store, TEST_DEVICE_A);
    writes_before = store.write_calls;
    store.fail_next_read_after_write = true;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) ==
           APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_SAVE_ADOPT_REQUIRED);
    assert(store.write_calls == writes_before + 1u);
    assert(receipt.opaque[0] == 0u && receipt.opaque[1] == 0u);
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    assert(store.write_calls == writes_before + 1u);
}

static void test_semantic_crosswire_rejected(void)
{
    struct fake_store store = {0};
    struct app_durable_state_gateway_assignment_identity identity =
        test_identity(7u);
    struct app_durable_state_gateway_assignment_identity crosswired;
    struct gateway_membership_snapshot snapshot = test_snapshot(&identity);
    struct gateway_membership_snapshot restored = {0};
    struct app_durable_state_gateway_assignment_identity restored_identity = {0};
    struct app_durable_receipt receipt = {0};
    struct fake_slot *slot;
    bool replay_debt = false;
    unsigned int writes_before;

    install_store(&store, TEST_DEVICE_A);
    writes_before = store.write_calls;
    crosswired = identity;
    crosswired.gateway_sequence++;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &crosswired, &receipt) == -EINVAL);
    assert(store.write_calls == writes_before);

    crosswired = identity;
    crosswired.host_session_id++;
    crosswired.correlation_id = crosswired.host_session_id;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &crosswired, &receipt) == -EINVAL);
    assert(store.write_calls == writes_before);

    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    slot = fake_find_slot(&store, TEST_GATEWAY_ASSIGNMENT_NVS_ID, false);
    assert(slot != NULL);
    test_put_u32(&slot->data[TEST_GATEWAY_SEQUENCE_OFFSET],
                 identity.gateway_sequence + 1u);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], 0u);
    test_put_u16(&slot->data[TEST_CRC_OFFSET],
                 test_crc16(slot->data, slot->len));
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &receipt) == -EPROTO);
    assert(!replay_debt);
    assert(restored.valid == 0u);
}

static void test_delete_readback_retry_is_identity_bound(void)
{
    struct fake_store store = {0};
    struct app_durable_state_gateway_assignment_identity identity =
        test_identity(5u);
    struct app_durable_state_gateway_assignment_identity wrong_identity =
        test_identity(6u);
    struct gateway_membership_snapshot snapshot = test_snapshot(&identity);
    struct app_durable_receipt receipt = {0};
    struct app_durable_receipt retired_receipt = {0};
    struct gateway_command_event terminal = terminal_event(&identity);
    struct gateway_membership_snapshot restored = {0};
    struct app_durable_state_gateway_assignment_identity restored_identity = {0};
    bool replay_debt = true;
    unsigned int erases_before;

    install_store(&store, TEST_DEVICE_A);
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    erases_before = store.erase_calls;
    /* A valid pending capability cannot decommission the only durable replay
     * source before its exact terminal host receipt retires the debt. */
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A, &identity, &receipt) == -EBUSY);
    assert(store.erase_calls == erases_before);
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A,
               &wrong_identity,
               &receipt) == -ESTALE);
    assert(app_durable_state_retire_gateway_assignment_commit(
               TEST_GATEWAY_A, &receipt, &terminal, &retired_receipt) == 0);
    erases_before = store.erase_calls;
    /* The old pending receipt cannot decommission the rewritten roster. */
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A, &identity, &receipt) == -ESTALE);
    assert(store.erase_calls == erases_before);
    store.fail_next_read_after_erase = true;
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A, &identity, &retired_receipt) == -ETIMEDOUT);
    assert(store.erase_calls == erases_before + 1u);
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A,
               &identity,
               &(struct app_durable_receipt){0}) == -ESTALE);
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A, &wrong_identity, &retired_receipt) == -ESTALE);
    assert(app_durable_state_delete_gateway_assignment(
               TEST_GATEWAY_A, &identity, &retired_receipt) == 0);
    assert(store.erase_calls == erases_before + 1u);
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &receipt) == 0);
    assert(!replay_debt);
    assert(restored.valid == 0u);
}

static void test_corruption_binding_and_readback_fail_closed(void)
{
    struct fake_store store = {0};
    struct app_durable_state_gateway_assignment_identity identity =
        test_identity(3u);
    struct gateway_membership_snapshot snapshot = test_snapshot(&identity);
    struct gateway_membership_snapshot restored = {0};
    struct app_durable_state_gateway_assignment_identity restored_identity = {0};
    struct app_durable_receipt receipt = {0};
    struct fake_slot *slot;
    bool replay_debt = true;
    unsigned int writes_before;

    install_store(&store, TEST_DEVICE_A);
    store.drop_next_write = true;
    writes_before = store.write_calls;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == -EIO);
    assert(store.write_calls == writes_before + 1u);
    assert(receipt.opaque[0] == 0u && receipt.opaque[1] == 0u);

    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    slot = fake_find_slot(&store, TEST_GATEWAY_ASSIGNMENT_NVS_ID, false);
    assert(slot != NULL && slot->len ==
           APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE);
    slot->data[100u] ^= 1u;
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &receipt) < 0);
    assert(!replay_debt);
    assert(restored.valid == 0u);
    assert(receipt.opaque[0] == 0u && receipt.opaque[1] == 0u);

    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) < 0);
    memset(slot->data, 0, sizeof(slot->data));
    slot->present = false;
    slot->len = 0u;
    assert(app_durable_state_save_gateway_assignment_commit(
               TEST_GATEWAY_A, &snapshot, &identity, &receipt) == 0);
    test_put_u64(&slot->data[TEST_GATEWAY_ID_OFFSET], TEST_GATEWAY_B);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], 0u);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], test_crc16(slot->data, slot->len));
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_A,
               &restored,
               &restored_identity,
               &replay_debt,
               &receipt) == -EACCES);

    slot = fake_find_slot(&store, TEST_BOOT_NVS_ID, false);
    assert(slot != NULL);
    memset(slot, 0, sizeof(*slot));
    install_store(&store, TEST_DEVICE_B);
    assert(app_durable_state_restore_gateway_assignment_commit(
               TEST_GATEWAY_B,
               &restored,
               &restored_identity,
               &replay_debt,
               &receipt) < 0);
}

int main(void)
{
    test_commit_retire_reboot_and_successor_admission();
    test_save_readback_retry_adopts_exact_commit();
    test_semantic_crosswire_rejected();
    test_delete_readback_retry_is_identity_bound();
    test_corruption_binding_and_readback_fail_closed();
    return 0;
}
