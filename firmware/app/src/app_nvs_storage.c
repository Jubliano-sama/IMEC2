#include "app_nvs_storage.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <limits.h>

LOG_MODULE_REGISTER(app_nvs_storage, LOG_LEVEL_INF);

#if defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP)

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(storage_partition), okay),
             "application persistence requires storage_partition");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) >=
                 2u * APP_NVS_STORAGE_SECTOR_SIZE,
             "application persistence requires at least two NVS sectors");
BUILD_ASSERT((DT_REG_ADDR(DT_NODELABEL(storage_partition)) %
              APP_NVS_STORAGE_SECTOR_SIZE) == 0u,
             "storage_partition must start on the shared NVS sector boundary");
BUILD_ASSERT((DT_REG_SIZE(DT_NODELABEL(storage_partition)) %
              APP_NVS_STORAGE_SECTOR_SIZE) == 0u,
             "storage_partition must contain whole shared NVS sectors");

static struct nvs_fs app_storage_nvs;
static atomic_t app_storage_ready = ATOMIC_INIT(0);
K_MUTEX_DEFINE(app_storage_init_lock);

int app_nvs_storage_init(void)
{
    const struct flash_area *area = NULL;
    const struct device *flash_device;
    struct flash_pages_info page;
    off_t area_offset;
    size_t area_size;
    int ret;

    if (atomic_get(&app_storage_ready) != 0) {
        return 0;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    ret = k_mutex_lock(&app_storage_init_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    if (atomic_get(&app_storage_ready) != 0) {
        ret = 0;
        goto out;
    }

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    if (ret < 0 || area == NULL) {
        ret = ret < 0 ? ret : -ENODEV;
        goto out;
    }
    flash_device = flash_area_get_device(area);
    area_offset = area->fa_off;
    area_size = area->fa_size;
    flash_area_close(area);

    if (!device_is_ready(flash_device)) {
        ret = -ENODEV;
        goto out;
    }
    ret = flash_get_page_info_by_offs(flash_device, area_offset, &page);
    if (ret < 0) {
        goto out;
    }
    if (page.size == 0u ||
        APP_NVS_STORAGE_SECTOR_SIZE % page.size != 0u ||
        area_offset % (off_t)APP_NVS_STORAGE_SECTOR_SIZE != 0 ||
        area_size % APP_NVS_STORAGE_SECTOR_SIZE != 0u ||
        area_size / APP_NVS_STORAGE_SECTOR_SIZE < 2u ||
        area_size / APP_NVS_STORAGE_SECTOR_SIZE > UINT16_MAX) {
        ret = -EINVAL;
        goto out;
    }

    app_storage_nvs.flash_device = flash_device;
    app_storage_nvs.offset = area_offset;
    app_storage_nvs.sector_size = APP_NVS_STORAGE_SECTOR_SIZE;
    app_storage_nvs.sector_count =
        (uint16_t)(area_size / APP_NVS_STORAGE_SECTOR_SIZE);
    ret = nvs_mount(&app_storage_nvs);
    if (ret < 0) {
        goto out;
    }
    atomic_set(&app_storage_ready, 1);
    LOG_INF("shared NVS mounted: offset=0x%08x sectors=%u sector_size=%u",
            (unsigned int)app_storage_nvs.offset,
            (unsigned int)app_storage_nvs.sector_count,
            (unsigned int)app_storage_nvs.sector_size);
    ret = 0;

out:
    k_mutex_unlock(&app_storage_init_lock);
    return ret;
}

bool app_nvs_storage_ready(void)
{
    return atomic_get(&app_storage_ready) != 0;
}

struct nvs_fs *app_nvs_storage_fs(void)
{
    return &app_storage_nvs;
}

#else

int app_nvs_storage_init(void)
{
    return -ENOTSUP;
}

bool app_nvs_storage_ready(void)
{
    return false;
}

struct nvs_fs *app_nvs_storage_fs(void)
{
    return NULL;
}

#endif
