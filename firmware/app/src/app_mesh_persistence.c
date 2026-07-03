#include "app_mesh_persistence.h"

#include "app_config.h"
#include "protocol.h"

#include <zephyr/sys/util.h>

#if (DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY) && \
    defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_persistence, LOG_LEVEL_INF);

#define APP_MESH_NVS_OUTBOX_ID 0x0101u
#define APP_MESH_NVS_COLLECTION_RESULT_ID 0x0102u
#define APP_MESH_NVS_CHILD_CUSTODY_ID 0x0103u
#define APP_MESH_NVS_GATEWAY_COLLECTION_ID 0x0104u
#define APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID 0x0105u
#define APP_MESH_NVS_SECTOR_SIZE 4096u

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(storage_partition), okay),
             "mesh persistence requires a storage_partition");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) >=
             (2u * APP_MESH_NVS_SECTOR_SIZE),
             "mesh persistence storage_partition must contain at least two NVS sectors");
BUILD_ASSERT(sizeof(struct mesh_relay_outbox_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh outbox snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_collection_result_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh collection result snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct mesh_relay_child_custody_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh child custody snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct gateway_collection_state_snapshot) <=
             (APP_MESH_NVS_SECTOR_SIZE - 256u),
             "gateway collection snapshot must leave NVS sector headroom");
BUILD_ASSERT(sizeof(struct gateway_membership_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway membership snapshot must fit comfortably in one NVS sector");

static struct nvs_fs mesh_nvs;
static bool mesh_nvs_ready;
static bool mesh_nvs_init_attempted;

int app_mesh_persistence_init(void)
{
    const struct flash_area *area = NULL;
    int ret;

    if (mesh_nvs_ready) {
        return 0;
    }
    if (mesh_nvs_init_attempted) {
        return -ENODEV;
    }
    mesh_nvs_init_attempted = true;

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    if (ret < 0 || area == NULL) {
        LOG_WRN("mesh persistence storage open failed: %d", ret);
        return ret < 0 ? ret : -ENODEV;
    }

    mesh_nvs.flash_device = area->fa_dev;
    mesh_nvs.offset = area->fa_off;
    mesh_nvs.sector_size = APP_MESH_NVS_SECTOR_SIZE;
    mesh_nvs.sector_count = area->fa_size / APP_MESH_NVS_SECTOR_SIZE;
    flash_area_close(area);

    if (!device_is_ready(mesh_nvs.flash_device) || mesh_nvs.sector_count < 2u) {
        LOG_WRN("mesh persistence flash not ready or too small");
        return -ENODEV;
    }

    ret = nvs_mount(&mesh_nvs);
    if (ret < 0) {
        LOG_WRN("mesh persistence NVS mount failed: %d", ret);
        return ret;
    }

    mesh_nvs_ready = true;
    LOG_INF("mesh persistence mounted: offset=0x%08x sectors=%u sector_size=%u",
            (unsigned int)mesh_nvs.offset,
            (unsigned int)mesh_nvs.sector_count,
            (unsigned int)mesh_nvs.sector_size);
    return 0;
}

static bool mesh_persistence_ready(void)
{
    return app_mesh_persistence_init() == 0;
}

void app_mesh_persistence_clear_outbox(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_OUTBOX_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted outbox clear failed: %d", ret);
    }
}

void app_mesh_persistence_clear_collection_result(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_COLLECTION_RESULT_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted collection result clear failed: %d", ret);
    }
}

void app_mesh_persistence_clear_child_custody(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_CHILD_CUSTODY_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted child custody clear failed: %d", ret);
    }
}

void app_mesh_persistence_clear_gateway_collection(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_GATEWAY_COLLECTION_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("gateway collection snapshot clear failed: %d", ret);
    }
}

void app_mesh_persistence_clear_gateway_membership(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
    if (!mesh_persistence_ready()) {
        return;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("gateway membership snapshot clear failed: %d", ret);
    }
}

int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    ssize_t written;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        app_mesh_persistence_clear_outbox();
        return 0;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh outbox snapshot export failed: %d", ret);
        return -EINVAL;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_OUTBOX_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (written < 0) {
        LOG_WRN("mesh outbox snapshot write failed: %d", (int)written);
        return (int)written;
    }
    if ((size_t)written != sizeof(snapshot)) {
        LOG_WRN("mesh outbox snapshot short write: %d/%u",
                (int)written,
                (unsigned int)sizeof(snapshot));
        return -EIO;
    }

    return 0;
}

int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    struct mesh_relay_child_custody_snapshot snapshot;
    ssize_t written;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = mesh_relay_export_child_custody_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        app_mesh_persistence_clear_child_custody();
        return 0;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh child custody snapshot export failed: %d", ret);
        return -EINVAL;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_CHILD_CUSTODY_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (written < 0) {
        LOG_WRN("mesh child custody snapshot write failed: %d", (int)written);
        return (int)written;
    }
    if ((size_t)written != sizeof(snapshot)) {
        LOG_WRN("mesh child custody snapshot short write: %d/%u",
                (int)written,
                (unsigned int)sizeof(snapshot));
        return -EIO;
    }

    return 0;
}

int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_OUTBOX_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
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
        app_mesh_persistence_clear_outbox();
        return -EINVAL;
    }

    ret = mesh_relay_restore_outbox_snapshot(relay, &snapshot, now_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh outbox snapshot restore rejected: %d", ret);
        app_mesh_persistence_clear_outbox();
        return -EINVAL;
    }

    LOG_INF("mesh outbox snapshot restored");
    return 0;
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
        app_mesh_persistence_clear_child_custody();
        return -EINVAL;
    }

    ret = mesh_relay_restore_child_custody_snapshot(relay, &snapshot, now_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh child custody snapshot restore rejected: %d", ret);
        app_mesh_persistence_clear_child_custody();
        return -EINVAL;
    }

    LOG_INF("mesh child custody snapshot restored");
    return 0;
}

int app_mesh_persistence_save_collection_result(
    const struct app_mesh_collection_result_snapshot *snapshot)
{
    struct app_mesh_collection_result_snapshot stored;
    ssize_t written;

    if (snapshot == NULL || !snapshot->valid ||
        snapshot->version != APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    stored = *snapshot;
    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_COLLECTION_RESULT_ID,
                        &stored,
                        sizeof(stored));
    if (written < 0) {
        LOG_WRN("mesh collection result snapshot write failed: %d", (int)written);
        return (int)written;
    }
    if ((size_t)written != sizeof(stored)) {
        LOG_WRN("mesh collection result snapshot short write: %d/%u",
                (int)written,
                (unsigned int)sizeof(stored));
        return -EIO;
    }

    return 0;
}

int app_mesh_persistence_save_gateway_collection(
    const struct gateway_collection_state *collection)
{
    struct gateway_collection_state_snapshot snapshot;
    ssize_t written;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = gateway_collection_export_snapshot(collection, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection snapshot export failed: %d", ret);
        return -EINVAL;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_COLLECTION_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (written < 0) {
        LOG_WRN("gateway collection snapshot write failed: %d", (int)written);
        return (int)written;
    }
    if ((size_t)written != sizeof(snapshot)) {
        LOG_WRN("gateway collection snapshot short write: %d/%u",
                (int)written,
                (unsigned int)sizeof(snapshot));
        return -EIO;
    }

    return 0;
}

int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster)
{
    struct gateway_membership_snapshot snapshot;
    ssize_t written;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = gateway_membership_export_snapshot(roster, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway membership snapshot export failed: %d", ret);
        return -EINVAL;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (written < 0) {
        LOG_WRN("gateway membership snapshot write failed: %d", (int)written);
        return (int)written;
    }
    if ((size_t)written != sizeof(snapshot)) {
        LOG_WRN("gateway membership snapshot short write: %d/%u",
                (int)written,
                (unsigned int)sizeof(snapshot));
        return -EIO;
    }

    return 0;
}

int app_mesh_persistence_restore_collection_result(
    struct app_mesh_collection_result_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_COLLECTION_RESULT_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("mesh collection result snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        snapshot->version != APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION ||
        !snapshot->valid) {
        LOG_WRN("mesh collection result snapshot rejected: size=%d version=%u valid=%u",
                (int)read_len,
                snapshot->version,
                snapshot->valid ? 1u : 0u);
        app_mesh_persistence_clear_collection_result();
        return -EINVAL;
    }

    return 0;
}

int app_mesh_persistence_restore_gateway_collection(
    struct gateway_collection_state *collection)
{
    struct gateway_collection_state_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (collection == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    gateway_collection_clear(collection);
    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_COLLECTION_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("gateway collection snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot)) {
        LOG_WRN("gateway collection snapshot has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(snapshot));
        app_mesh_persistence_clear_gateway_collection();
        return -EINVAL;
    }

    ret = gateway_collection_restore_snapshot(collection, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection snapshot restore rejected: %d", ret);
        app_mesh_persistence_clear_gateway_collection();
        return -EINVAL;
    }

    LOG_INF("gateway collection snapshot restored: command_seq=%u collection=%u received=%u expected=%u open=%u",
            collection->command_seq,
            collection->collection_epoch_id,
            collection->received_count,
            collection->expected_count,
            collection->collection_open ? 1u : 0u);
    return 0;
}

int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster)
{
    struct gateway_membership_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (roster == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        gateway_membership_clear(roster);
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    gateway_membership_clear(roster);
    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("gateway membership snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot)) {
        LOG_WRN("gateway membership snapshot has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(snapshot));
        app_mesh_persistence_clear_gateway_membership();
        return -EINVAL;
    }

    ret = gateway_membership_restore_snapshot(roster, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway membership snapshot restore rejected: %d", ret);
        app_mesh_persistence_clear_gateway_membership();
        return -EINVAL;
    }

    LOG_INF("gateway membership snapshot restored: epoch=%u nodes=%u",
            roster->membership_epoch,
            roster->node_count);
    return 0;
}

#if defined(CONFIG_ZTEST)
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
    return (size_t)written == snapshot_len ? 0 : -EIO;
}
#endif

#else

#include <errno.h>

int app_mesh_persistence_init(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

void app_mesh_persistence_clear_outbox(void)
{
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

void app_mesh_persistence_clear_child_custody(void)
{
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

void app_mesh_persistence_clear_collection_result(void)
{
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

void app_mesh_persistence_clear_gateway_collection(void)
{
}

int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster)
{
    ARG_UNUSED(roster);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster)
{
    gateway_membership_clear(roster);
    return -ENOTSUP;
}

void app_mesh_persistence_clear_gateway_membership(void)
{
}

#endif
