#include "app_mesh_route_state_persistence.h"

#include "app_nvs_storage.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define APP_MESH_ROUTE_STATE_MAGIC UINT32_C(0x52544550)
#define APP_MESH_ROUTE_STATE_VERSION 1u
#define APP_MESH_ROUTE_STATE_VALID UINT8_C(0xA5)

struct app_mesh_route_state_record {
    uint64_t local_id;
    uint64_t gateway_id;
    uint32_t magic;
    uint32_t route_epoch;
    uint32_t gateway_route_adv_seq;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint8_t role;
    uint8_t valid;
    uint32_t reserved;
};

BUILD_ASSERT(sizeof(struct app_mesh_route_state_record) ==
                 APP_NVS_ROUTE_STATE_RECORD_SIZE,
             "route-state persistence capacity model drifted");
BUILD_ASSERT(offsetof(struct app_mesh_route_state_record, checksum) == 32u,
             "route-state persistence checksum layout drifted");
BUILD_ASSERT(offsetof(struct app_mesh_route_state_record, reserved) == 36u,
             "route-state persistence tail layout drifted");

#if defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP)

static K_MUTEX_DEFINE(route_state_lock);

static bool route_state_id_is_unicast(uint64_t id)
{
    return id != 0u && id != UINT64_MAX;
}

static int route_state_record_id(enum mesh_relay_role role, uint16_t *id)
{
    if (id == NULL) {
        return -EINVAL;
    }

    switch (role) {
    case MESH_RELAY_ROLE_CLICKER:
        *id = APP_NVS_ID_MESH_ROUTE_STATE_CLICKER;
        return 0;
    case MESH_RELAY_ROLE_ANCHOR:
        *id = APP_NVS_ID_MESH_ROUTE_STATE_ANCHOR;
        return 0;
    case MESH_RELAY_ROLE_GATEWAY:
        *id = APP_NVS_ID_MESH_ROUTE_STATE_GATEWAY;
        return 0;
    default:
        return -EINVAL;
    }
}

static uint16_t route_state_checksum(
    const struct app_mesh_route_state_record *record)
{
    struct app_mesh_route_state_record copy;

    if (record == NULL) {
        return 0u;
    }
    copy = *record;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static int route_state_validate_record(
    const struct app_mesh_route_state_record *record)
{
    enum mesh_relay_role role;

    if (record == NULL ||
        record->magic != APP_MESH_ROUTE_STATE_MAGIC ||
        record->version != APP_MESH_ROUTE_STATE_VERSION ||
        record->size != sizeof(*record) ||
        record->valid != APP_MESH_ROUTE_STATE_VALID ||
        record->reserved != 0u ||
        record->checksum != route_state_checksum(record) ||
        !route_state_id_is_unicast(record->local_id) ||
        !route_state_id_is_unicast(record->gateway_id) ||
        record->route_epoch == 0u ||
        (uint16_t)record->route_epoch == 0u) {
        return -EILSEQ;
    }

    role = (enum mesh_relay_role)record->role;
    if ((role != MESH_RELAY_ROLE_CLICKER &&
         role != MESH_RELAY_ROLE_ANCHOR &&
         role != MESH_RELAY_ROLE_GATEWAY) ||
        ((role == MESH_RELAY_ROLE_GATEWAY) !=
         (record->local_id == record->gateway_id)) ||
        (role != MESH_RELAY_ROLE_ANCHOR &&
         record->gateway_route_adv_seq != 0u)) {
        return -EILSEQ;
    }
    return 0;
}

static int route_state_validate_identity(
    const struct app_mesh_route_state_record *record,
    const struct mesh_relay *relay)
{
    if (record == NULL || relay == NULL) {
        return -EINVAL;
    }
    if (record->role != (uint8_t)relay->role ||
        record->local_id != relay->local_id ||
        record->gateway_id != relay->gateway_id) {
        return -ESTALE;
    }
    return 0;
}

static int route_state_storage_init(void)
{
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    return app_nvs_storage_init();
}

int app_mesh_route_state_persist(const struct mesh_relay *relay,
                                 uint32_t route_epoch,
                                 uint32_t gateway_route_adv_seq)
{
    struct app_mesh_route_state_record verify;
    struct app_mesh_route_state_record record;
    struct nvs_fs *fs;
    uint16_t record_id;
    ssize_t io_len;
    int ret;

    if (relay == NULL ||
        !route_state_id_is_unicast(relay->local_id) ||
        !route_state_id_is_unicast(relay->gateway_id) ||
        ((relay->role == MESH_RELAY_ROLE_GATEWAY) !=
         (relay->local_id == relay->gateway_id)) ||
        route_epoch == 0u ||
        (uint16_t)route_epoch == 0u ||
        (relay->role != MESH_RELAY_ROLE_ANCHOR &&
         gateway_route_adv_seq != 0u)) {
        return -EINVAL;
    }
    ret = route_state_record_id(relay->role, &record_id);
    if (ret < 0) {
        return ret;
    }
    ret = route_state_storage_init();
    if (ret < 0) {
        return ret;
    }
    fs = app_nvs_storage_fs();
    if (fs == NULL) {
        return -ENODEV;
    }

    record = (struct app_mesh_route_state_record) {
        .local_id = relay->local_id,
        .gateway_id = relay->gateway_id,
        .magic = APP_MESH_ROUTE_STATE_MAGIC,
        .route_epoch = route_epoch,
        .gateway_route_adv_seq = gateway_route_adv_seq,
        .version = APP_MESH_ROUTE_STATE_VERSION,
        .size = sizeof(record),
        .role = (uint8_t)relay->role,
        .valid = APP_MESH_ROUTE_STATE_VALID,
    };
    record.checksum = route_state_checksum(&record);

    ret = k_mutex_lock(&route_state_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    io_len = nvs_write(fs, record_id, &record, sizeof(record));
    if (io_len < 0) {
        ret = (int)io_len;
        goto out;
    }
    if (io_len != 0 && io_len != sizeof(record)) {
        ret = -EIO;
        goto out;
    }

    io_len = nvs_read(fs, record_id, &verify, sizeof(verify));
    if (io_len < 0) {
        ret = (int)io_len;
        goto out;
    }
    if (io_len != sizeof(verify) ||
        memcmp(&verify, &record, sizeof(record)) != 0 ||
        route_state_validate_record(&verify) < 0) {
        ret = -EIO;
        goto out;
    }
    ret = 0;

out:
    k_mutex_unlock(&route_state_lock);
    return ret;
}

int app_mesh_route_state_save(const struct mesh_relay *relay)
{
    if (relay == NULL) {
        return -EINVAL;
    }
    return app_mesh_route_state_persist(relay,
                                        relay->upstream.current_epoch,
                                        relay->gateway_route_adv_seq);
}

int app_mesh_route_state_restore(struct mesh_relay *relay)
{
    struct app_mesh_route_state_record record;
    struct nvs_fs *fs;
    uint16_t record_id;
    ssize_t read_len;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    ret = route_state_record_id(relay->role, &record_id);
    if (ret < 0) {
        return ret;
    }
    ret = route_state_storage_init();
    if (ret < 0) {
        return ret;
    }
    fs = app_nvs_storage_fs();
    if (fs == NULL) {
        return -ENODEV;
    }

    ret = k_mutex_lock(&route_state_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    read_len = nvs_read(fs, record_id, &record, sizeof(record));
    k_mutex_unlock(&route_state_lock);
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if (read_len != sizeof(record)) {
        return -EILSEQ;
    }
    ret = route_state_validate_record(&record);
    if (ret < 0) {
        return ret;
    }
    ret = route_state_validate_identity(&record, relay);
    if (ret < 0) {
        return ret;
    }
    ret = mesh_relay_restore_route_freshness(
        relay, record.route_epoch, record.gateway_route_adv_seq);
    if (ret != PROTO_OK) {
        return -EILSEQ;
    }
    return 1;
}

int app_mesh_route_state_clear(uint8_t role)
{
    struct nvs_fs *fs;
    uint16_t record_id;
    int ret;

    ret = route_state_record_id((enum mesh_relay_role)role, &record_id);
    if (ret < 0) {
        return ret;
    }
    ret = route_state_storage_init();
    if (ret < 0) {
        return ret;
    }
    fs = app_nvs_storage_fs();
    if (fs == NULL) {
        return -ENODEV;
    }

    ret = k_mutex_lock(&route_state_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    ret = nvs_delete(fs, record_id);
    k_mutex_unlock(&route_state_lock);
    return ret;
}

#else

int app_mesh_route_state_persist(const struct mesh_relay *relay,
                                 uint32_t route_epoch,
                                 uint32_t gateway_route_adv_seq)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(route_epoch);
    ARG_UNUSED(gateway_route_adv_seq);
    return -ENOTSUP;
}

int app_mesh_route_state_save(const struct mesh_relay *relay)
{
    ARG_UNUSED(relay);
    return -ENOTSUP;
}

int app_mesh_route_state_restore(struct mesh_relay *relay)
{
    ARG_UNUSED(relay);
    return 0;
}

int app_mesh_route_state_clear(uint8_t role)
{
    ARG_UNUSED(role);
    return -ENOTSUP;
}

#endif
