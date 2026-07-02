#include "app_mesh_persistence.h"

#include "app_config.h"
#include "protocol.h"

#include <zephyr/sys/util.h>

#if DEVICE_ROLE == ROLE_ANCHOR

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_persistence, LOG_LEVEL_INF);

#define APP_MESH_NVS_OUTBOX_ID 0x0101u
#define APP_MESH_NVS_SECTOR_SIZE 4096u

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(storage_partition), okay),
             "mesh persistence requires a storage_partition");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) >=
             (2u * APP_MESH_NVS_SECTOR_SIZE),
             "mesh persistence storage_partition must contain at least two NVS sectors");
BUILD_ASSERT(sizeof(struct mesh_relay_outbox_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh outbox snapshot must fit comfortably in one NVS sector");

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

#endif
