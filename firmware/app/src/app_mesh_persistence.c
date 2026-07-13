#include "app_mesh_persistence.h"
#include "discovery_assignment.h"

#include "app_config.h"
#include "protocol.h"

#include <zephyr/sys/util.h>

#if (DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY) && \
    defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_persistence, LOG_LEVEL_INF);

#define APP_MESH_NVS_OUTBOX_ID 0x0101u
#define APP_MESH_NVS_COLLECTION_RESULT_ID 0x0102u
#define APP_MESH_NVS_CHILD_CUSTODY_ID 0x0103u
#define APP_MESH_NVS_GATEWAY_COLLECTION_ID 0x0104u
#define APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID 0x0105u
#define APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID 0x0106u
#define APP_MESH_NVS_CLICK_HANDOFF_ID 0x0107u
#define APP_MESH_NVS_LOCAL_DELIVERY_ID 0x0108u
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
BUILD_ASSERT(sizeof(struct app_mesh_click_handoff_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh click handoff snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_local_delivery_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "local delivery journal must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct gateway_collection_state_snapshot) <=
             (APP_MESH_NVS_SECTOR_SIZE - 256u),
             "gateway collection snapshot must leave NVS sector headroom");
BUILD_ASSERT(sizeof(struct gateway_membership_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway membership snapshot must fit comfortably in one NVS sector");

static struct nvs_fs mesh_nvs;
static bool mesh_nvs_ready;
static uint8_t mesh_nvs_init_retry_round;
static uint32_t mesh_nvs_init_retry_at_ms;
static struct app_mesh_persistence_health mesh_persistence_health;

static void mesh_persistence_note_failure(int ret)
{
    if (mesh_persistence_health.total_failures < UINT32_MAX) {
        mesh_persistence_health.total_failures++;
    }
    if (mesh_persistence_health.consecutive_failures < UINT16_MAX) {
        mesh_persistence_health.consecutive_failures++;
    }
    mesh_persistence_health.last_error = ret;
    mesh_persistence_health.ready = mesh_nvs_ready;
}

static void mesh_persistence_note_success(void)
{
    mesh_persistence_health.consecutive_failures = 0u;
    mesh_persistence_health.last_error = 0;
    mesh_persistence_health.ready = mesh_nvs_ready;
}

static int mesh_persistence_init_failed(int ret)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        mesh_nvs_init_retry_round,
        sys_rand32_get());

    mesh_nvs_ready = false;
    if (mesh_nvs_init_retry_round < UINT8_MAX) {
        mesh_nvs_init_retry_round++;
    }
    mesh_nvs_init_retry_at_ms = k_uptime_get_32() + delay_ms;
    mesh_persistence_note_failure(ret);
    LOG_WRN("mesh persistence recovery scheduled: ret=%d round=%u delay_ms=%u",
            ret,
            mesh_nvs_init_retry_round,
            delay_ms);
    return ret;
}

void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health)
{
    if (health != NULL) {
        *health = mesh_persistence_health;
    }
}

int app_mesh_persistence_init(void)
{
    const struct flash_area *area = NULL;
    int ret;

    if (mesh_nvs_ready) {
        return 0;
    }
    if (mesh_nvs_init_retry_at_ms != 0u &&
        (int32_t)(k_uptime_get_32() - mesh_nvs_init_retry_at_ms) < 0) {
        return mesh_persistence_health.last_error == 0 ?
               -EAGAIN : mesh_persistence_health.last_error;
    }

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    if (ret < 0 || area == NULL) {
        LOG_WRN("mesh persistence storage open failed: %d", ret);
        return mesh_persistence_init_failed(ret < 0 ? ret : -ENODEV);
    }

    mesh_nvs.flash_device = area->fa_dev;
    mesh_nvs.offset = area->fa_off;
    mesh_nvs.sector_size = APP_MESH_NVS_SECTOR_SIZE;
    mesh_nvs.sector_count = area->fa_size / APP_MESH_NVS_SECTOR_SIZE;
    flash_area_close(area);

    if (!device_is_ready(mesh_nvs.flash_device) || mesh_nvs.sector_count < 2u) {
        LOG_WRN("mesh persistence flash not ready or too small");
        return mesh_persistence_init_failed(-ENODEV);
    }

    ret = nvs_mount(&mesh_nvs);
    if (ret < 0) {
        LOG_WRN("mesh persistence NVS mount failed: %d", ret);
        return mesh_persistence_init_failed(ret);
    }

    mesh_nvs_ready = true;
    mesh_nvs_init_retry_round = 0u;
    mesh_nvs_init_retry_at_ms = 0u;
    mesh_persistence_note_success();
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

static int mesh_persistence_write(uint16_t id,
                                  const void *data,
                                  size_t len,
                                  const char *label)
{
    ssize_t written = nvs_write(&mesh_nvs, id, data, len);
    int ret;

    if (written < 0) {
        ret = (int)written;
    } else if ((size_t)written != len) {
        ret = -EIO;
    } else {
        mesh_persistence_note_success();
        return 0;
    }
    mesh_persistence_note_failure(ret);
    LOG_WRN("%s write failed: ret=%d written=%d expected=%u",
            label == NULL ? "mesh persistence" : label,
            ret,
            (int)written,
            (unsigned int)len);
    return ret;
}

static bool click_handoff_snapshot_valid(
    const struct app_mesh_click_handoff_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->valid &&
           snapshot->version == APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION &&
           (snapshot->phase == APP_MESH_CLICK_HANDOFF_STAGED ||
            snapshot->phase == APP_MESH_CLICK_HANDOFF_COMMITTED) &&
           snapshot->outbox.valid;
}

static bool outbox_snapshots_match(const struct mesh_relay_outbox_snapshot *left,
                                   const struct mesh_relay_outbox_snapshot *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->role == right->role && left->local_id == right->local_id &&
           left->gateway_id == right->gateway_id &&
           left->pending.packet.msg_type == right->pending.packet.msg_type &&
           left->pending.packet.src_id == right->pending.packet.src_id &&
           left->pending.packet.dst_id == right->pending.packet.dst_id &&
           left->pending.packet.session_id == right->pending.packet.session_id &&
           left->pending.packet.seq == right->pending.packet.seq &&
           left->pending.payload_len == right->pending.payload_len &&
           memcmp(left->pending.payload,
                  right->pending.payload,
                  left->pending.payload_len) == 0;
}

static int read_click_handoff(struct app_mesh_click_handoff_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_CLICK_HANDOFF_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !click_handoff_snapshot_valid(snapshot)) {
        mesh_persistence_note_failure(-EINVAL);
        return -EINVAL;
    }
    return 1;
}

static int clear_click_handoff(void)
{
    int ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_CLICK_HANDOFF_ID);

    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_outbox(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_OUTBOX_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted outbox clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (snapshot == NULL ||
        !app_mesh_local_delivery_snapshot_valid(snapshot)) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    return mesh_persistence_write(APP_MESH_NVS_LOCAL_DELIVERY_ID,
                                  snapshot, sizeof(*snapshot),
                                  "local delivery journal");
}

int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    read_len = nvs_read(&mesh_nvs, APP_MESH_NVS_LOCAL_DELIVERY_ID,
                        snapshot, sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !app_mesh_local_delivery_snapshot_valid(snapshot)) {
        memset(snapshot, 0, sizeof(*snapshot));
        return -EBADMSG;
    }
    return 1;
}

int app_mesh_persistence_clear_local_delivery(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_LOCAL_DELIVERY_ID);
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot = {
        .version = APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION,
        .phase = APP_MESH_CLICK_HANDOFF_STAGED,
        .valid = true,
    };
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot.outbox);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NOT_FOUND ? -ENOENT : -EINVAL;
    }
    return mesh_persistence_write(APP_MESH_NVS_CLICK_HANDOFF_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh click handoff stage");
}

int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot;
    struct mesh_relay_outbox_snapshot active;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = read_click_handoff(&snapshot);
    if (ret <= 0) {
        return ret == 0 ? -ENOENT : ret;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &active);
    if (ret != PROTO_OK || !outbox_snapshots_match(&snapshot.outbox, &active)) {
        return -ESTALE;
    }
    snapshot.phase = APP_MESH_CLICK_HANDOFF_COMMITTED;
    return mesh_persistence_write(APP_MESH_NVS_CLICK_HANDOFF_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh click handoff commit");
}

int app_mesh_persistence_rollback_click_handoff(void)
{
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    return clear_click_handoff();
}

int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot;
    struct mesh_relay_outbox_snapshot active;
    int ret;

    if (relay == NULL || !mesh_relay_tx_active(relay)) {
        return 0;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = read_click_handoff(&snapshot);
    if (ret <= 0) {
        return ret < 0 ? ret : 0;
    }
    if (snapshot.phase != APP_MESH_CLICK_HANDOFF_COMMITTED) {
        return 0;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &active);
    if (ret != PROTO_OK || !outbox_snapshots_match(&snapshot.outbox, &active)) {
        return 0;
    }
    return clear_click_handoff();
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
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
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

    return mesh_persistence_write(APP_MESH_NVS_OUTBOX_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh outbox snapshot");
}

int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    struct mesh_relay_child_custody_snapshot snapshot;
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
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

    return mesh_persistence_write(APP_MESH_NVS_CHILD_CUSTODY_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh child custody snapshot");
}

int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    struct app_mesh_click_handoff_snapshot handoff;
    ssize_t read_len;
    int handoff_ret;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    handoff_ret = read_click_handoff(&handoff);
    if (handoff_ret < 0) {
        LOG_WRN("mesh click handoff journal read failed: %d", handoff_ret);
        return handoff_ret;
    }
    if (handoff_ret > 0 && handoff.phase == APP_MESH_CLICK_HANDOFF_COMMITTED) {
        ret = mesh_relay_restore_outbox_snapshot(relay, &handoff.outbox, now_ms);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh committed click handoff restore rejected: %d", ret);
            return -EINVAL;
        }
        /* The committed journal is authoritative until the same outbox is resaved. */
        ret = app_mesh_persistence_clear_outbox();
        if (ret < 0) {
            LOG_WRN("mesh obsolete outbox clear deferred after handoff restore: %d", ret);
        }
        LOG_INF("mesh committed click handoff restored");
        return 0;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_OUTBOX_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        if (handoff_ret > 0) {
            ret = mesh_relay_restore_outbox_snapshot(relay, &handoff.outbox, now_ms);
            if (ret != PROTO_OK) {
                LOG_WRN("mesh staged click handoff fallback restore rejected: %d", ret);
                return -EINVAL;
            }
            LOG_INF("mesh staged click handoff restored as reset fallback");
        }
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
    if (handoff_ret > 0) {
        ret = clear_click_handoff();
        if (ret < 0) {
            LOG_WRN("mesh staged click handoff cleanup deferred: %d", ret);
        }
    }
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
    int ret;

    if (snapshot == NULL || !snapshot->valid ||
        snapshot->version != APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    stored = *snapshot;
    return mesh_persistence_write(APP_MESH_NVS_COLLECTION_RESULT_ID,
                                  &stored,
                                  sizeof(stored),
                                  "mesh collection result snapshot");
}

int app_mesh_persistence_save_gateway_collection(
    const struct gateway_collection_state *collection)
{
    struct gateway_collection_state_snapshot snapshot;
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = gateway_collection_export_snapshot(collection, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection snapshot export failed: %d", ret);
        return -EINVAL;
    }

    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_COLLECTION_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "gateway collection snapshot");
}

int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster)
{
    struct gateway_membership_snapshot snapshot;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = gateway_membership_export_snapshot(roster, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway membership snapshot export failed: %d", ret);
        return -EINVAL;
    }

    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "gateway membership snapshot");
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

int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    int ret;

    if (snapshot == NULL || !snapshot->valid ||
        snapshot->version != APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION ||
        snapshot->epoch == 0u || snapshot->local_id == 0u ||
        snapshot->gateway_id == 0u || snapshot->slot_count == 0u ||
        snapshot->slot >= snapshot->slot_count) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    return mesh_persistence_write(APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID,
                                  snapshot,
                                  sizeof(*snapshot),
                                  "discovery assignment snapshot");
}

int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    ssize_t read_len;
    int ret;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0 || (size_t)read_len != sizeof(*snapshot) ||
        !snapshot->valid ||
        snapshot->version != APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION ||
        snapshot->epoch == 0u || snapshot->slot_count == 0u ||
        snapshot->slot >= snapshot->slot_count) {
        memset(snapshot, 0, sizeof(*snapshot));
        return read_len < 0 ? (int)read_len : -EINVAL;
    }
    return 0;
}

void app_mesh_persistence_clear_discovery_assignment(void)
{
    if (mesh_persistence_ready()) {
        int ret = nvs_delete(&mesh_nvs,
                             APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID);

        if (ret < 0 && ret != -ENOENT) {
            mesh_persistence_note_failure(ret);
        }
    }
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
#include <string.h>

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

int app_mesh_persistence_clear_outbox(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_local_delivery(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_rollback_click_handoff(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
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

int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

void app_mesh_persistence_clear_discovery_assignment(void)
{
}

void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health)
{
    if (health != NULL) {
        memset(health, 0, sizeof(*health));
    }
}

#endif
