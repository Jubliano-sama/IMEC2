#include "app_durable_state.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* One NVS ATE, data CRC, and worst-case four-byte write padding. */
#define APP_DURABLE_STATE_NVS_ENTRY_OVERHEAD 16u

#if !defined(APP_DURABLE_STATE_TESTING)
#include "app_config.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if !defined(CONFIG_IMEC_DURABLE_STATE)
#error "app_durable_state.c requires CONFIG_IMEC_DURABLE_STATE"
#endif
#if !defined(DEVICE_ROLE)
#error "production durable state requires a compiled DEVICE_ROLE"
#endif

#define APP_DURABLE_STATE_SECTOR_SIZE 4096u

BUILD_ASSERT(IS_ENABLED(CONFIG_FLASH),
             "durable state requires the flash driver");
BUILD_ASSERT(IS_ENABLED(CONFIG_FLASH_MAP),
             "durable state requires the flash map");
BUILD_ASSERT(IS_ENABLED(CONFIG_FLASH_PAGE_LAYOUT),
             "durable state requires the flash page layout API");
BUILD_ASSERT(IS_ENABLED(CONFIG_NVS),
             "durable state requires NVS");
BUILD_ASSERT(IS_ENABLED(CONFIG_NVS_DATA_CRC),
             "durable state requires per-entry NVS data CRC");
BUILD_ASSERT(ROLE_CLICKER == APP_DURABLE_STATE_ROLE_CLICKER &&
                 ROLE_ANCHOR == APP_DURABLE_STATE_ROLE_ANCHOR &&
                 ROLE_GATEWAY == APP_DURABLE_STATE_ROLE_GATEWAY,
             "durable-state roles must match the application role ABI");
BUILD_ASSERT(DEVICE_ROLE == APP_DURABLE_STATE_ROLE_CLICKER ||
                 DEVICE_ROLE == APP_DURABLE_STATE_ROLE_ANCHOR ||
                 DEVICE_ROLE == APP_DURABLE_STATE_ROLE_GATEWAY,
             "durable state requires one exact compiled application role");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(storage_partition), okay),
             "durable state requires storage_partition");
BUILD_ASSERT(CONFIG_FLASH_LOAD_SIZE > 0u,
             "durable builds must cap the linked image before storage");
BUILD_ASSERT(CONFIG_FLASH_LOAD_OFFSET + CONFIG_FLASH_LOAD_SIZE <=
                 DT_REG_ADDR(DT_NODELABEL(storage_partition)),
             "linked image may not overlap storage_partition");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) >=
                 2u * APP_DURABLE_STATE_SECTOR_SIZE,
             "durable state requires at least two NVS sectors");
BUILD_ASSERT(APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE +
                 APP_DURABLE_STATE_NVS_ENTRY_OVERHEAD <=
                 APP_DURABLE_STATE_SECTOR_SIZE,
             "gateway assignment record plus NVS metadata must fit a sector");
BUILD_ASSERT((DT_REG_ADDR(DT_NODELABEL(storage_partition)) %
              APP_DURABLE_STATE_SECTOR_SIZE) == 0u,
             "storage_partition must start on an NVS sector boundary");
BUILD_ASSERT((DT_REG_SIZE(DT_NODELABEL(storage_partition)) %
              APP_DURABLE_STATE_SECTOR_SIZE) == 0u,
             "storage_partition must contain complete NVS sectors");
#endif

#define APP_DURABLE_STATE_MAGIC UINT32_C(0x44555231)
#define APP_DURABLE_STATE_VALID_FLAG 1u

#define APP_DURABLE_STATE_ID_GATEWAY_COMMAND UINT16_C(0x010f)
#define APP_DURABLE_STATE_ID_SURVEY_GENERATION UINT16_C(0x01a1)
#define APP_DURABLE_STATE_ID_CLICK_SEQUENCE UINT16_C(0x0201)
#define APP_DURABLE_STATE_ID_BOOT_INCARNATION UINT16_C(0x0300)
#define APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT UINT16_C(0x0301)
#define APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT UINT16_C(0x0302)

#define APP_DURABLE_STATE_ROLE_BIT(role) \
    ((uint8_t)(1u << (uint8_t)(role)))

#define RECORD_MAGIC_OFFSET 0u
#define RECORD_SCHEMA_OFFSET 4u
#define RECORD_TOTAL_SIZE_OFFSET 6u
#define RECORD_TYPE_OFFSET 8u
#define RECORD_VERSION_OFFSET 10u
#define RECORD_ROLE_OFFSET 12u
#define RECORD_FLAGS_OFFSET 13u
#define RECORD_RESERVED16_OFFSET 14u
#define RECORD_DEVICE_ID_OFFSET 16u
#define RECORD_PAYLOAD_SIZE_OFFSET 24u
#define RECORD_CRC_OFFSET 26u
#define RECORD_RESERVED32_OFFSET 28u
#define RECORD_SCOPE_ID_OFFSET APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define RECORD_HIGH_WATER_OFFSET \
    (APP_DURABLE_STATE_RECORD_HEADER_SIZE + sizeof(uint64_t))

#define ASSIGNMENT_GATEWAY_ID_OFFSET APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define ASSIGNMENT_TABLE_COMMITMENT_OFFSET \
    (ASSIGNMENT_GATEWAY_ID_OFFSET + sizeof(uint64_t))
#define ASSIGNMENT_PENDING_COMMITMENT_OFFSET \
    (ASSIGNMENT_TABLE_COMMITMENT_OFFSET + SEMANTIC_DIGEST_SHA256_LEN)
#define ASSIGNMENT_EPOCH_OFFSET \
    (ASSIGNMENT_PENDING_COMMITMENT_OFFSET + SEMANTIC_DIGEST_SHA256_LEN)
#define ASSIGNMENT_TABLE_COMMAND_SEQ_OFFSET \
    (ASSIGNMENT_EPOCH_OFFSET + sizeof(uint32_t))
#define ASSIGNMENT_PENDING_EPOCH_OFFSET \
    (ASSIGNMENT_TABLE_COMMAND_SEQ_OFFSET + sizeof(uint32_t))
#define ASSIGNMENT_PENDING_TABLE_COMMAND_SEQ_OFFSET \
    (ASSIGNMENT_PENDING_EPOCH_OFFSET + sizeof(uint32_t))
#define ASSIGNMENT_RETIRED_EPOCHS_OFFSET \
    (ASSIGNMENT_PENDING_TABLE_COMMAND_SEQ_OFFSET + sizeof(uint32_t))
#define ASSIGNMENT_TABLE_PACKET_SEQ_OFFSET \
    (ASSIGNMENT_RETIRED_EPOCHS_OFFSET + \
     DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP * sizeof(uint32_t))
#define ASSIGNMENT_RESPONSE_SPREAD_MS_OFFSET \
    (ASSIGNMENT_TABLE_PACKET_SEQ_OFFSET + sizeof(uint16_t))
#define ASSIGNMENT_SLOT_OFFSET \
    (ASSIGNMENT_RESPONSE_SPREAD_MS_OFFSET + sizeof(uint16_t))
#define ASSIGNMENT_SLOT_COUNT_OFFSET (ASSIGNMENT_SLOT_OFFSET + 1u)
#define ASSIGNMENT_PROVISIONED_OFFSET (ASSIGNMENT_SLOT_COUNT_OFFSET + 1u)
#define ASSIGNMENT_RETIRED_EPOCH_COUNT_OFFSET \
    (ASSIGNMENT_PROVISIONED_OFFSET + 1u)
#define ASSIGNMENT_ORDERED_EPOCH_VALID_OFFSET \
    (ASSIGNMENT_RETIRED_EPOCH_COUNT_OFFSET + 1u)
#define ASSIGNMENT_ACK_PENDING_OFFSET \
    (ASSIGNMENT_ORDERED_EPOCH_VALID_OFFSET + 1u)
#define ASSIGNMENT_PENDING_SLOT_OFFSET (ASSIGNMENT_ACK_PENDING_OFFSET + 1u)
#define ASSIGNMENT_PENDING_SLOT_COUNT_OFFSET \
    (ASSIGNMENT_PENDING_SLOT_OFFSET + 1u)
#define ASSIGNMENT_PENDING_VALID_OFFSET \
    (ASSIGNMENT_PENDING_SLOT_COUNT_OFFSET + 1u)
#define ASSIGNMENT_PENDING_RESPONSE_LANE_OFFSET \
    (ASSIGNMENT_PENDING_VALID_OFFSET + 1u)
#define ASSIGNMENT_PENDING_RESPONSE_LANE_COUNT_OFFSET \
    (ASSIGNMENT_PENDING_RESPONSE_LANE_OFFSET + 1u)
#define ASSIGNMENT_RESERVED_OFFSET \
    (ASSIGNMENT_PENDING_RESPONSE_LANE_COUNT_OFFSET + 1u)

#define GATEWAY_ASSIGNMENT_GATEWAY_ID_OFFSET \
    APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define GATEWAY_ASSIGNMENT_IDENTITY_OFFSET \
    (GATEWAY_ASSIGNMENT_GATEWAY_ID_OFFSET + sizeof(uint64_t))
#define GATEWAY_ASSIGNMENT_CORRELATION_ID_OFFSET \
    GATEWAY_ASSIGNMENT_IDENTITY_OFFSET
#define GATEWAY_ASSIGNMENT_GATEWAY_SEQUENCE_OFFSET \
    (GATEWAY_ASSIGNMENT_CORRELATION_ID_OFFSET + sizeof(uint32_t))
#define GATEWAY_ASSIGNMENT_HOST_SESSION_ID_OFFSET \
    (GATEWAY_ASSIGNMENT_GATEWAY_SEQUENCE_OFFSET + sizeof(uint32_t))
#define GATEWAY_ASSIGNMENT_GATEWAY_EPOCH_OFFSET \
    (GATEWAY_ASSIGNMENT_HOST_SESSION_ID_OFFSET + sizeof(uint32_t))
#define GATEWAY_ASSIGNMENT_HOST_SEQ_OFFSET \
    (GATEWAY_ASSIGNMENT_GATEWAY_EPOCH_OFFSET + sizeof(uint16_t))
#define GATEWAY_ASSIGNMENT_SNAPSHOT_OFFSET \
    (GATEWAY_ASSIGNMENT_HOST_SEQ_OFFSET + sizeof(uint16_t))
#define GATEWAY_ASSIGNMENT_RETIRED_FROM_FINGERPRINT_OFFSET \
    (GATEWAY_ASSIGNMENT_SNAPSHOT_OFFSET + \
     GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE)

_Static_assert(ASSIGNMENT_RESERVED_OFFSET + 1u ==
                   APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE,
               "anchor assignment canonical record size changed");
_Static_assert(GATEWAY_ASSIGNMENT_SNAPSHOT_OFFSET +
                   GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE +
                   APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RETIRE_PROOF_SIZE ==
                   APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE,
               "gateway assignment canonical record size changed");
_Static_assert(APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE +
                   APP_DURABLE_STATE_NVS_ENTRY_OVERHEAD <= 4096u,
               "gateway assignment record must fit one minimum NVS sector");

struct durable_backend {
    void *context;
    int (*mount)(void *context);
    ssize_t (*read)(void *context,
                    uint16_t id,
                    void *data,
                    size_t len);
    ssize_t (*write)(void *context,
                     uint16_t id,
                     const void *data,
                     size_t len);
    int (*erase)(void *context, uint16_t id);
};

struct durable_counter_spec {
    enum app_durable_state_record_type type;
    uint16_t nvs_id;
    uint8_t restore_roles;
    uint8_t reserve_roles;
    uint8_t advance_roles;
    uint64_t first_install_floor;
    uint32_t reservation_count;
    uint32_t high_water_alignment;
    bool scope_required;
    bool counter_is_u32;
    bool wraps_without_zero;
    bool skip_low_u16_zero;
    bool skip_low_u32_zero;
};

static const struct durable_counter_spec durable_counter_specs[] = {
    {
        .type = APP_DURABLE_STATE_BOOT_INCARNATION,
        .nvs_id = APP_DURABLE_STATE_ID_BOOT_INCARNATION,
        .restore_roles = APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_CLICKER) |
                         APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_ANCHOR) |
                         APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_GATEWAY),
        .reserve_roles = APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_CLICKER) |
                         APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_ANCHOR) |
                         APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_GATEWAY),
        .first_install_floor = 1u,
        .reservation_count = 1u,
        .counter_is_u32 = true,
        .skip_low_u16_zero = true,
    },
    {
        .type = APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
        .nvs_id = APP_DURABLE_STATE_ID_CLICK_SEQUENCE,
        .restore_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_CLICKER),
        .reserve_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_CLICKER),
        .first_install_floor =
            APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR,
        .reservation_count = APP_DURABLE_STATE_CLICK_BLOCK_SIZE,
        .high_water_alignment = APP_DURABLE_STATE_CLICK_BLOCK_SIZE,
        .counter_is_u32 = true,
    },
    {
        .type = APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
        .nvs_id = APP_DURABLE_STATE_ID_GATEWAY_COMMAND,
        .restore_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_GATEWAY),
        .reserve_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_GATEWAY),
        .first_install_floor =
            APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR,
        .reservation_count = APP_DURABLE_STATE_COMMAND_BLOCK_SIZE,
        .high_water_alignment = APP_DURABLE_STATE_COMMAND_BLOCK_SIZE,
        .counter_is_u32 = true,
    },
    {
        .type = APP_DURABLE_STATE_SURVEY_GENERATION,
        .nvs_id = APP_DURABLE_STATE_ID_SURVEY_GENERATION,
        .restore_roles = APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_ANCHOR) |
                         APP_DURABLE_STATE_ROLE_BIT(
                             APP_DURABLE_STATE_ROLE_GATEWAY),
        .reserve_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_GATEWAY),
        .advance_roles = APP_DURABLE_STATE_ROLE_BIT(
            APP_DURABLE_STATE_ROLE_ANCHOR),
        .first_install_floor = 0u,
        .reservation_count = 1u,
        .scope_required = true,
        .skip_low_u32_zero = true,
    },
};

struct durable_owner {
    struct durable_backend backend;
    enum app_durable_state_role role;
    uint64_t device_id;
    uint32_t boot_incarnation;
    bool backend_installed;
    bool mounted;
};

static struct durable_owner durable_owner;

/*
 * A gateway journal is larger than any worker stack allowance.  These buffers
 * are serialized by durable_lock() and only exist in the gateway image (or a
 * native durable-state test binary), never on a BLE/persistence worker stack.
 */
#if defined(APP_DURABLE_STATE_TESTING)
#define DURABLE_GATEWAY_ASSIGNMENT_ENABLED 1
#elif DEVICE_ROLE == ROLE_GATEWAY
#define DURABLE_GATEWAY_ASSIGNMENT_ENABLED 1
#else
#define DURABLE_GATEWAY_ASSIGNMENT_ENABLED 0
#endif

#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
static uint8_t durable_gateway_assignment_record[
    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE];
static struct gateway_membership_snapshot durable_gateway_assignment_snapshot;
#endif

#if defined(APP_DURABLE_STATE_TESTING)
static bool durable_ready;

static int durable_lock(void)
{
    return 0;
}

static void durable_unlock(void)
{
}

static bool durable_ready_get(void)
{
    return durable_ready;
}

static void durable_ready_set(bool ready)
{
    durable_ready = ready;
}
#else
static struct nvs_fs durable_nvs;
static atomic_t durable_ready = ATOMIC_INIT(0);
K_MUTEX_DEFINE(durable_state_mutex);

static int durable_nvs_mount(void *context)
{
    struct nvs_fs *fs = context;
    const struct flash_area *area = NULL;
    const struct device *flash_device;
    struct flash_pages_info page;
    off_t area_offset;
    size_t area_size;
    int ret;

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    if (ret < 0 || area == NULL) {
        return ret < 0 ? ret : -ENODEV;
    }
    flash_device = flash_area_get_device(area);
    area_offset = area->fa_off;
    area_size = area->fa_size;
    flash_area_close(area);

    if (!device_is_ready(flash_device)) {
        return -ENODEV;
    }
    ret = flash_get_page_info_by_offs(flash_device, area_offset, &page);
    if (ret < 0) {
        return ret;
    }
    if (page.size == 0u ||
        page.start_offset != area_offset ||
        APP_DURABLE_STATE_SECTOR_SIZE % page.size != 0u ||
        area_offset % (off_t)APP_DURABLE_STATE_SECTOR_SIZE != 0 ||
        area_size % APP_DURABLE_STATE_SECTOR_SIZE != 0u ||
        area_size / APP_DURABLE_STATE_SECTOR_SIZE < 2u ||
        area_size / APP_DURABLE_STATE_SECTOR_SIZE > UINT16_MAX) {
        return -EINVAL;
    }

    memset(fs, 0, sizeof(*fs));
    fs->flash_device = flash_device;
    fs->offset = area_offset;
    fs->sector_size = APP_DURABLE_STATE_SECTOR_SIZE;
    fs->sector_count =
        (uint16_t)(area_size / APP_DURABLE_STATE_SECTOR_SIZE);
    return nvs_mount(fs);
}

static ssize_t durable_nvs_read(void *context,
                                uint16_t id,
                                void *data,
                                size_t len)
{
    return nvs_read(context, id, data, len);
}

static ssize_t durable_nvs_write(void *context,
                                 uint16_t id,
                                 const void *data,
                                 size_t len)
{
    return nvs_write(context, id, data, len);
}

static int durable_nvs_erase(void *context, uint16_t id)
{
    return nvs_delete(context, id);
}

static int durable_lock(void)
{
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    return k_mutex_lock(&durable_state_mutex, K_FOREVER);
}

static void durable_unlock(void)
{
    k_mutex_unlock(&durable_state_mutex);
}

static bool durable_ready_get(void)
{
    return atomic_get(&durable_ready) != 0;
}

static void durable_ready_set(bool ready)
{
    atomic_set(&durable_ready, ready ? 1 : 0);
}

static void durable_install_production_backend(void)
{
    if (durable_owner.backend_installed) {
        return;
    }
    durable_owner.backend = (struct durable_backend) {
        .context = &durable_nvs,
        .mount = durable_nvs_mount,
        .read = durable_nvs_read,
        .write = durable_nvs_write,
        .erase = durable_nvs_erase,
    };
    durable_owner.role = (enum app_durable_state_role)DEVICE_ROLE;
    durable_owner.backend_installed = true;
}
#endif

static void durable_put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void durable_put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void durable_put_u64(uint8_t *dst, uint64_t value)
{
    for (uint8_t i = 0u; i < sizeof(value); i++) {
        dst[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t durable_get_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t durable_get_u32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t durable_get_u64(const uint8_t *src)
{
    uint64_t value = 0u;

    for (uint8_t i = 0u; i < sizeof(value); i++) {
        value |= (uint64_t)src[i] << (8u * i);
    }
    return value;
}

static uint16_t durable_crc16(const uint8_t *data, size_t len)
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

static const struct durable_counter_spec *durable_find_spec(
    enum app_durable_state_record_type type)
{
    for (size_t i = 0u;
         i < sizeof(durable_counter_specs) / sizeof(durable_counter_specs[0]);
         i++) {
        if (durable_counter_specs[i].type == type) {
            return &durable_counter_specs[i];
        }
    }
    return NULL;
}

static bool durable_role_allowed(uint8_t role_mask)
{
    if (durable_owner.role < APP_DURABLE_STATE_ROLE_CLICKER ||
        durable_owner.role > APP_DURABLE_STATE_ROLE_GATEWAY) {
        return false;
    }
    return (role_mask & APP_DURABLE_STATE_ROLE_BIT(durable_owner.role)) != 0u;
}

static int durable_validate_scope(const struct durable_counter_spec *spec,
                                  uint64_t scope_id)
{
    if (spec == NULL) {
        return -EINVAL;
    }
    if (spec->scope_required) {
        return scope_id == 0u ? -EINVAL : 0;
    }
    return scope_id == 0u ? 0 : -EINVAL;
}

static int durable_validate_high_water(
    const struct durable_counter_spec *spec,
    uint64_t high_water)
{
    if (spec == NULL || high_water == 0u) {
        return -EILSEQ;
    }
    if (spec->counter_is_u32 && high_water > UINT32_MAX) {
        return -EILSEQ;
    }
    if (!spec->wraps_without_zero &&
        high_water < spec->first_install_floor) {
        return -EILSEQ;
    }
    if (spec->high_water_alignment != 0u &&
        (high_water % spec->high_water_alignment) !=
            (spec->first_install_floor % spec->high_water_alignment)) {
        return -EILSEQ;
    }
    if (spec->skip_low_u32_zero && (uint32_t)high_water == 0u) {
        return -EILSEQ;
    }
    if (spec->skip_low_u16_zero && (uint16_t)high_water == 0u) {
        return -EILSEQ;
    }
    return 0;
}

static void durable_encode_record(
    uint8_t record[APP_DURABLE_STATE_COUNTER_RECORD_SIZE],
    const struct durable_counter_spec *spec,
    uint64_t scope_id,
    uint64_t high_water)
{
    memset(record, 0, APP_DURABLE_STATE_COUNTER_RECORD_SIZE);
    durable_put_u32(&record[RECORD_MAGIC_OFFSET], APP_DURABLE_STATE_MAGIC);
    durable_put_u16(&record[RECORD_SCHEMA_OFFSET],
                    APP_DURABLE_STATE_ENVELOPE_VERSION);
    durable_put_u16(&record[RECORD_TOTAL_SIZE_OFFSET],
                    APP_DURABLE_STATE_COUNTER_RECORD_SIZE);
    durable_put_u16(&record[RECORD_TYPE_OFFSET], (uint16_t)spec->type);
    durable_put_u16(&record[RECORD_VERSION_OFFSET],
                    APP_DURABLE_STATE_RECORD_VERSION);
    record[RECORD_ROLE_OFFSET] = (uint8_t)durable_owner.role;
    record[RECORD_FLAGS_OFFSET] = APP_DURABLE_STATE_VALID_FLAG;
    durable_put_u64(&record[RECORD_DEVICE_ID_OFFSET], durable_owner.device_id);
    durable_put_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET],
                    APP_DURABLE_STATE_COUNTER_PAYLOAD_SIZE);
    durable_put_u64(&record[RECORD_SCOPE_ID_OFFSET], scope_id);
    durable_put_u64(&record[RECORD_HIGH_WATER_OFFSET], high_water);
    durable_put_u16(&record[RECORD_CRC_OFFSET],
                    durable_crc16(record,
                                  APP_DURABLE_STATE_COUNTER_RECORD_SIZE));
}

static int durable_decode_record(
    uint8_t record[APP_DURABLE_STATE_COUNTER_RECORD_SIZE],
    size_t record_len,
    const struct durable_counter_spec *spec,
    uint64_t expected_scope_id,
    uint64_t *high_water)
{
    uint16_t stored_crc;
    uint64_t stored_scope_id;
    uint64_t stored_high_water;
    int ret;

    if (record == NULL || spec == NULL || high_water == NULL) {
        return -EINVAL;
    }
    *high_water = 0u;
    if (record_len != APP_DURABLE_STATE_COUNTER_RECORD_SIZE) {
        return -EILSEQ;
    }
    if (durable_get_u32(&record[RECORD_MAGIC_OFFSET]) !=
            APP_DURABLE_STATE_MAGIC ||
        durable_get_u16(&record[RECORD_SCHEMA_OFFSET]) !=
            APP_DURABLE_STATE_ENVELOPE_VERSION ||
        durable_get_u16(&record[RECORD_TOTAL_SIZE_OFFSET]) != record_len ||
        durable_get_u16(&record[RECORD_TYPE_OFFSET]) !=
            (uint16_t)spec->type ||
        durable_get_u16(&record[RECORD_VERSION_OFFSET]) !=
            APP_DURABLE_STATE_RECORD_VERSION ||
        record[RECORD_FLAGS_OFFSET] != APP_DURABLE_STATE_VALID_FLAG ||
        durable_get_u16(&record[RECORD_RESERVED16_OFFSET]) != 0u ||
        durable_get_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET]) !=
            APP_DURABLE_STATE_COUNTER_PAYLOAD_SIZE ||
        durable_get_u32(&record[RECORD_RESERVED32_OFFSET]) != 0u) {
        return -EPROTO;
    }
    if (record[RECORD_ROLE_OFFSET] != (uint8_t)durable_owner.role ||
        durable_get_u64(&record[RECORD_DEVICE_ID_OFFSET]) !=
            durable_owner.device_id) {
        return -EACCES;
    }

    stored_crc = durable_get_u16(&record[RECORD_CRC_OFFSET]);
    durable_put_u16(&record[RECORD_CRC_OFFSET], 0u);
    if (stored_crc !=
        durable_crc16(record, APP_DURABLE_STATE_COUNTER_RECORD_SIZE)) {
        return -EBADMSG;
    }
    durable_put_u16(&record[RECORD_CRC_OFFSET], stored_crc);

    stored_scope_id = durable_get_u64(&record[RECORD_SCOPE_ID_OFFSET]);
    if (stored_scope_id != expected_scope_id) {
        return -EACCES;
    }
    stored_high_water = durable_get_u64(&record[RECORD_HIGH_WATER_OFFSET]);
    ret = durable_validate_high_water(spec, stored_high_water);
    if (ret < 0) {
        return ret;
    }
    *high_water = stored_high_water;
    return 0;
}

static int durable_read_locked(const struct durable_counter_spec *spec,
                               uint64_t scope_id,
                               uint64_t *high_water)
{
    uint8_t record[APP_DURABLE_STATE_COUNTER_RECORD_SIZE];
    ssize_t read_len;
    int ret;

    memset(record, 0, sizeof(record));
    read_len = durable_owner.backend.read(durable_owner.backend.context,
                                          spec->nvs_id,
                                          record,
                                          sizeof(record));
    if (read_len == -ENOENT) {
        *high_water = 0u;
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    ret = durable_decode_record(record,
                                (size_t)read_len,
                                spec,
                                scope_id,
                                high_water);
    return ret < 0 ? ret : 1;
}

static int durable_write_locked(const struct durable_counter_spec *spec,
                                uint64_t scope_id,
                                uint64_t high_water)
{
    uint8_t record[APP_DURABLE_STATE_COUNTER_RECORD_SIZE];
    uint64_t verified_high_water = 0u;
    ssize_t io_len;
    int ret;

    ret = durable_validate_high_water(spec, high_water);
    if (ret < 0) {
        return ret;
    }
    durable_encode_record(record, spec, scope_id, high_water);
    io_len = durable_owner.backend.write(durable_owner.backend.context,
                                         spec->nvs_id,
                                         record,
                                         sizeof(record));
    if (io_len < 0) {
        return (int)io_len;
    }
    if (io_len != 0 && (size_t)io_len != sizeof(record)) {
        return -EIO;
    }

    memset(record, 0, sizeof(record));
    io_len = durable_owner.backend.read(durable_owner.backend.context,
                                        spec->nvs_id,
                                        record,
                                        sizeof(record));
    if (io_len < 0) {
        return io_len == -ENOENT ? -EIO : (int)io_len;
    }
    ret = durable_decode_record(record,
                                (size_t)io_len,
                                spec,
                                scope_id,
                                &verified_high_water);
    if (ret < 0) {
        return ret;
    }
    return verified_high_water == high_water ? 0 : -EIO;
}

static bool durable_assignment_commitment_is_zero(
    const struct discovery_assignment_table_commitment *commitment)
{
    uint8_t combined = 0u;

    if (commitment == NULL) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(commitment->bytes); i++) {
        combined |= commitment->bytes[i];
    }
    return combined == 0u;
}

static bool durable_assignment_history_valid(
    const struct app_durable_state_anchor_assignment *assignment)
{
    uint32_t freshness_epoch;

    if (assignment == NULL ||
        assignment->retired_epoch_count >
            DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP) {
        return false;
    }
    freshness_epoch = assignment->pending_valid != 0u ?
                      assignment->pending_epoch : assignment->epoch;
    if (freshness_epoch == 0u) {
        return false;
    }
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        uint32_t retired = assignment->retired_epochs[i];

        if (i >= assignment->retired_epoch_count) {
            if (retired != 0u) {
                return false;
            }
            continue;
        }
        if (retired == 0u || retired == freshness_epoch ||
            (assignment->epoch != 0u &&
             (retired == assignment->epoch ||
              !discovery_assignment_epoch_strictly_newer(
                  assignment->epoch, retired))) ||
            !discovery_assignment_epoch_strictly_newer(freshness_epoch,
                                                       retired) ||
            (i != 0u &&
             !discovery_assignment_epoch_strictly_newer(
                 assignment->retired_epochs[i - 1u], retired))) {
            return false;
        }
        for (size_t prior = 0u; prior < i; prior++) {
            if (assignment->retired_epochs[prior] == retired) {
                return false;
            }
        }
    }
    return true;
}

static int durable_validate_anchor_assignment(
    const struct app_durable_state_anchor_assignment *assignment)
{
    bool finalized_identity_present;
    bool pending_identity_present;

    if (assignment == NULL || assignment->provisioned > 1u ||
        assignment->ordered_epoch_valid != 1u ||
        assignment->ack_pending > 1u || assignment->pending_valid > 1u) {
        return -EINVAL;
    }
    finalized_identity_present =
        assignment->epoch != 0u && assignment->table_command_seq != 0u;
    pending_identity_present =
        assignment->pending_epoch != 0u &&
        assignment->pending_table_command_seq != 0u;
    if ((assignment->epoch == 0u) !=
            (assignment->table_command_seq == 0u) ||
        (assignment->pending_epoch == 0u) !=
            (assignment->pending_table_command_seq == 0u) ||
        (!finalized_identity_present && assignment->pending_valid == 0u) ||
        (!finalized_identity_present &&
         assignment->retired_epoch_count != 0u)) {
        return -EINVAL;
    }
    if ((finalized_identity_present &&
         durable_assignment_commitment_is_zero(
             &assignment->table_commitment)) ||
        (!finalized_identity_present &&
         !durable_assignment_commitment_is_zero(
             &assignment->table_commitment))) {
        return -EINVAL;
    }
    if (assignment->provisioned != 0u) {
        if (!finalized_identity_present || assignment->slot_count == 0u ||
            assignment->slot_count > UWB_DISCOVERY_SLOT_COUNT ||
            assignment->slot >= assignment->slot_count) {
            return -EINVAL;
        }
    } else if (!finalized_identity_present) {
        if (assignment->slot != 0u || assignment->slot_count != 0u) {
            return -EINVAL;
        }
    }

    if (assignment->pending_valid != 0u) {
        if (!pending_identity_present ||
            durable_assignment_commitment_is_zero(
                &assignment->pending_table_commitment) ||
            assignment->pending_slot_count == 0u ||
            assignment->pending_slot_count > UWB_DISCOVERY_SLOT_COUNT ||
            assignment->pending_response_lane_count >
                UWB_DISCOVERY_SLOT_COUNT ||
            (assignment->pending_response_lane_count != 0u &&
             assignment->pending_response_lane >=
                 assignment->pending_response_lane_count) ||
            (finalized_identity_present &&
             !discovery_assignment_epoch_strictly_newer(
                 assignment->pending_epoch, assignment->epoch))) {
            return -EINVAL;
        }
    } else if (pending_identity_present ||
               !durable_assignment_commitment_is_zero(
                   &assignment->pending_table_commitment) ||
               assignment->pending_slot != 0u ||
               assignment->pending_slot_count != 0u ||
               assignment->pending_response_lane != 0u ||
               assignment->pending_response_lane_count != 0u) {
        return -EINVAL;
    }

    if (assignment->ack_pending != 0u) {
        if (assignment->pending_valid == 0u ||
            assignment->pending_slot >= assignment->pending_slot_count ||
            assignment->response_spread_ms <
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
            assignment->response_spread_ms >
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS ||
            assignment->table_packet_seq == 0u) {
            return -EINVAL;
        }
    } else if (assignment->table_packet_seq != 0u ||
               assignment->response_spread_ms != 0u ||
               (assignment->pending_valid != 0u &&
                assignment->pending_slot != 0u)) {
        return -EINVAL;
    }
    return durable_assignment_history_valid(assignment) ? 0 : -EINVAL;
}

static bool durable_anchor_assignments_equal(
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

static void durable_encode_anchor_assignment(
    uint8_t record[APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE],
    uint64_t gateway_id,
    const struct app_durable_state_anchor_assignment *assignment)
{
    memset(record, 0, APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE);
    durable_put_u32(&record[RECORD_MAGIC_OFFSET], APP_DURABLE_STATE_MAGIC);
    durable_put_u16(&record[RECORD_SCHEMA_OFFSET],
                    APP_DURABLE_STATE_ENVELOPE_VERSION);
    durable_put_u16(&record[RECORD_TOTAL_SIZE_OFFSET],
                    APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE);
    durable_put_u16(&record[RECORD_TYPE_OFFSET],
                    APP_DURABLE_STATE_ANCHOR_ASSIGNMENT);
    durable_put_u16(&record[RECORD_VERSION_OFFSET],
                    APP_DURABLE_STATE_RECORD_VERSION);
    record[RECORD_ROLE_OFFSET] = APP_DURABLE_STATE_ROLE_ANCHOR;
    record[RECORD_FLAGS_OFFSET] = APP_DURABLE_STATE_VALID_FLAG;
    durable_put_u64(&record[RECORD_DEVICE_ID_OFFSET], durable_owner.device_id);
    durable_put_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET],
                    APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE);
    durable_put_u64(&record[ASSIGNMENT_GATEWAY_ID_OFFSET], gateway_id);
    memcpy(&record[ASSIGNMENT_TABLE_COMMITMENT_OFFSET],
           assignment->table_commitment.bytes,
           sizeof(assignment->table_commitment.bytes));
    memcpy(&record[ASSIGNMENT_PENDING_COMMITMENT_OFFSET],
           assignment->pending_table_commitment.bytes,
           sizeof(assignment->pending_table_commitment.bytes));
    durable_put_u32(&record[ASSIGNMENT_EPOCH_OFFSET], assignment->epoch);
    durable_put_u32(&record[ASSIGNMENT_TABLE_COMMAND_SEQ_OFFSET],
                    assignment->table_command_seq);
    durable_put_u32(&record[ASSIGNMENT_PENDING_EPOCH_OFFSET],
                    assignment->pending_epoch);
    durable_put_u32(&record[ASSIGNMENT_PENDING_TABLE_COMMAND_SEQ_OFFSET],
                    assignment->pending_table_command_seq);
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        durable_put_u32(
            &record[ASSIGNMENT_RETIRED_EPOCHS_OFFSET +
                    i * sizeof(uint32_t)],
            assignment->retired_epochs[i]);
    }
    durable_put_u16(&record[ASSIGNMENT_TABLE_PACKET_SEQ_OFFSET],
                    assignment->table_packet_seq);
    durable_put_u16(&record[ASSIGNMENT_RESPONSE_SPREAD_MS_OFFSET],
                    assignment->response_spread_ms);
    record[ASSIGNMENT_SLOT_OFFSET] = assignment->slot;
    record[ASSIGNMENT_SLOT_COUNT_OFFSET] = assignment->slot_count;
    record[ASSIGNMENT_PROVISIONED_OFFSET] = assignment->provisioned;
    record[ASSIGNMENT_RETIRED_EPOCH_COUNT_OFFSET] =
        assignment->retired_epoch_count;
    record[ASSIGNMENT_ORDERED_EPOCH_VALID_OFFSET] =
        assignment->ordered_epoch_valid;
    record[ASSIGNMENT_ACK_PENDING_OFFSET] = assignment->ack_pending;
    record[ASSIGNMENT_PENDING_SLOT_OFFSET] = assignment->pending_slot;
    record[ASSIGNMENT_PENDING_SLOT_COUNT_OFFSET] =
        assignment->pending_slot_count;
    record[ASSIGNMENT_PENDING_VALID_OFFSET] = assignment->pending_valid;
    record[ASSIGNMENT_PENDING_RESPONSE_LANE_OFFSET] =
        assignment->pending_response_lane;
    record[ASSIGNMENT_PENDING_RESPONSE_LANE_COUNT_OFFSET] =
        assignment->pending_response_lane_count;
    durable_put_u16(
        &record[RECORD_CRC_OFFSET],
        durable_crc16(record,
                      APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE));
}

static int durable_decode_anchor_assignment(
    uint8_t record[APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE],
    size_t record_len,
    uint64_t expected_gateway_id,
    struct app_durable_state_anchor_assignment *assignment)
{
    uint16_t stored_crc;
    int ret;

    if (record == NULL || assignment == NULL) {
        return -EINVAL;
    }
    memset(assignment, 0, sizeof(*assignment));
    if (record_len != APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE) {
        return -EILSEQ;
    }
    if (durable_get_u32(&record[RECORD_MAGIC_OFFSET]) !=
            APP_DURABLE_STATE_MAGIC ||
        durable_get_u16(&record[RECORD_SCHEMA_OFFSET]) !=
            APP_DURABLE_STATE_ENVELOPE_VERSION ||
        durable_get_u16(&record[RECORD_TOTAL_SIZE_OFFSET]) != record_len ||
        durable_get_u16(&record[RECORD_TYPE_OFFSET]) !=
            APP_DURABLE_STATE_ANCHOR_ASSIGNMENT ||
        durable_get_u16(&record[RECORD_VERSION_OFFSET]) !=
            APP_DURABLE_STATE_RECORD_VERSION ||
        record[RECORD_FLAGS_OFFSET] != APP_DURABLE_STATE_VALID_FLAG ||
        durable_get_u16(&record[RECORD_RESERVED16_OFFSET]) != 0u ||
        durable_get_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET]) !=
            APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE ||
        durable_get_u32(&record[RECORD_RESERVED32_OFFSET]) != 0u ||
        record[ASSIGNMENT_RESERVED_OFFSET] != 0u) {
        return -EPROTO;
    }
    if (record[RECORD_ROLE_OFFSET] != APP_DURABLE_STATE_ROLE_ANCHOR ||
        durable_owner.role != APP_DURABLE_STATE_ROLE_ANCHOR ||
        durable_get_u64(&record[RECORD_DEVICE_ID_OFFSET]) !=
            durable_owner.device_id ||
        durable_get_u64(&record[ASSIGNMENT_GATEWAY_ID_OFFSET]) !=
            expected_gateway_id) {
        return -EACCES;
    }
    stored_crc = durable_get_u16(&record[RECORD_CRC_OFFSET]);
    durable_put_u16(&record[RECORD_CRC_OFFSET], 0u);
    if (stored_crc !=
        durable_crc16(record,
                      APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE)) {
        return -EBADMSG;
    }
    durable_put_u16(&record[RECORD_CRC_OFFSET], stored_crc);

    memcpy(assignment->table_commitment.bytes,
           &record[ASSIGNMENT_TABLE_COMMITMENT_OFFSET],
           sizeof(assignment->table_commitment.bytes));
    memcpy(assignment->pending_table_commitment.bytes,
           &record[ASSIGNMENT_PENDING_COMMITMENT_OFFSET],
           sizeof(assignment->pending_table_commitment.bytes));
    assignment->epoch = durable_get_u32(&record[ASSIGNMENT_EPOCH_OFFSET]);
    assignment->table_command_seq =
        durable_get_u32(&record[ASSIGNMENT_TABLE_COMMAND_SEQ_OFFSET]);
    assignment->pending_epoch =
        durable_get_u32(&record[ASSIGNMENT_PENDING_EPOCH_OFFSET]);
    assignment->pending_table_command_seq = durable_get_u32(
        &record[ASSIGNMENT_PENDING_TABLE_COMMAND_SEQ_OFFSET]);
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        assignment->retired_epochs[i] = durable_get_u32(
            &record[ASSIGNMENT_RETIRED_EPOCHS_OFFSET +
                    i * sizeof(uint32_t)]);
    }
    assignment->table_packet_seq =
        durable_get_u16(&record[ASSIGNMENT_TABLE_PACKET_SEQ_OFFSET]);
    assignment->response_spread_ms =
        durable_get_u16(&record[ASSIGNMENT_RESPONSE_SPREAD_MS_OFFSET]);
    assignment->slot = record[ASSIGNMENT_SLOT_OFFSET];
    assignment->slot_count = record[ASSIGNMENT_SLOT_COUNT_OFFSET];
    assignment->provisioned = record[ASSIGNMENT_PROVISIONED_OFFSET];
    assignment->retired_epoch_count =
        record[ASSIGNMENT_RETIRED_EPOCH_COUNT_OFFSET];
    assignment->ordered_epoch_valid =
        record[ASSIGNMENT_ORDERED_EPOCH_VALID_OFFSET];
    assignment->ack_pending = record[ASSIGNMENT_ACK_PENDING_OFFSET];
    assignment->pending_slot = record[ASSIGNMENT_PENDING_SLOT_OFFSET];
    assignment->pending_slot_count =
        record[ASSIGNMENT_PENDING_SLOT_COUNT_OFFSET];
    assignment->pending_valid = record[ASSIGNMENT_PENDING_VALID_OFFSET];
    assignment->pending_response_lane =
        record[ASSIGNMENT_PENDING_RESPONSE_LANE_OFFSET];
    assignment->pending_response_lane_count =
        record[ASSIGNMENT_PENDING_RESPONSE_LANE_COUNT_OFFSET];
    ret = durable_validate_anchor_assignment(assignment);
    if (ret < 0) {
        memset(assignment, 0, sizeof(*assignment));
        return -EPROTO;
    }
    return 0;
}

static void durable_receipt_clear(struct app_durable_receipt *receipt)
{
    if (receipt != NULL) {
        memset(receipt, 0, sizeof(*receipt));
    }
}

#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
#define DURABLE_RECEIPT_DOMAIN_RECORD UINT64_C(0x4455525245433031)
#define DURABLE_RECEIPT_DOMAIN_BINDING UINT64_C(0x4455525245433032)

static bool durable_gateway_assignment_identity_valid(
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    return identity != NULL && identity->correlation_id != 0u &&
           identity->gateway_sequence != 0u &&
           identity->host_session_id != 0u && identity->gateway_epoch != 0u &&
           identity->host_seq != 0u;
}

/* The snapshot is the TABLE/roster proof for this exact command event, not
 * merely an independently well-formed roster.  Keep these relationships at
 * the storage boundary so a caller cannot mint a capability for cross-wired
 * semantic inputs. */
static bool durable_gateway_assignment_snapshot_identity_coherent(
    uint64_t gateway_id,
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    if (gateway_id == 0u || snapshot == NULL ||
        !durable_gateway_assignment_identity_valid(identity) ||
        identity->correlation_id != identity->host_session_id ||
        snapshot->assignment_proof_valid == 0u ||
        snapshot->assignment_epoch != identity->gateway_sequence ||
        snapshot->membership_epoch !=
            discovery_assignment_membership_epoch(identity->gateway_sequence)) {
        return false;
    }

    return snapshot->publication_table_round == 0u ||
           (snapshot->publication_host_dst_id == gateway_id &&
            snapshot->publication_host_session_id == identity->host_session_id &&
            snapshot->publication_host_seq == identity->host_seq &&
            snapshot->publication_event_gateway_epoch == identity->gateway_epoch);
}

static bool durable_gateway_assignment_identities_equal(
    const struct app_durable_state_gateway_assignment_identity *left,
    const struct app_durable_state_gateway_assignment_identity *right)
{
    return left != NULL && right != NULL &&
           left->correlation_id == right->correlation_id &&
           left->gateway_sequence == right->gateway_sequence &&
           left->host_session_id == right->host_session_id &&
           left->gateway_epoch == right->gateway_epoch &&
           left->host_seq == right->host_seq;
}

static uint64_t durable_receipt_mix(uint64_t value, uint64_t input)
{
    for (uint8_t byte = 0u; byte < sizeof(input); byte++) {
        value ^= (uint8_t)(input >> (8u * byte));
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static uint64_t durable_record_fingerprint(const uint8_t *record,
                                           size_t record_len,
                                           uint64_t domain)
{
    uint64_t value = domain;

    if (record == NULL) {
        return 0u;
    }
    for (size_t index = 0u; index < record_len; index++) {
        value ^= record[index];
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static uint64_t durable_gateway_assignment_receipt_binding(
    uint64_t gateway_id,
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    uint64_t value = DURABLE_RECEIPT_DOMAIN_BINDING;

    if (identity == NULL) {
        return 0u;
    }
    value = durable_receipt_mix(value, durable_owner.device_id);
    value = durable_receipt_mix(value, gateway_id);
    value = durable_receipt_mix(value, identity->correlation_id);
    value = durable_receipt_mix(value, identity->gateway_sequence);
    value = durable_receipt_mix(value, identity->host_session_id);
    value = durable_receipt_mix(value, identity->gateway_epoch);
    return durable_receipt_mix(value, identity->host_seq);
}

static void durable_gateway_assignment_issue_receipt(
    const uint8_t record[APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE],
    uint64_t gateway_id,
    const struct app_durable_state_gateway_assignment_identity *identity,
    struct app_durable_receipt *receipt)
{
    if (receipt == NULL) {
        return;
    }
    receipt->opaque[0] = durable_record_fingerprint(
        record,
        APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE,
        DURABLE_RECEIPT_DOMAIN_RECORD);
    receipt->opaque[1] = durable_gateway_assignment_receipt_binding(
        gateway_id, identity);
}

static bool durable_gateway_assignment_receipt_matches(
    const uint8_t record[APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE],
    uint64_t gateway_id,
    const struct app_durable_state_gateway_assignment_identity *identity,
    const struct app_durable_receipt *receipt)
{
    return receipt != NULL &&
           receipt->opaque[0] == durable_record_fingerprint(
               record,
               APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE,
               DURABLE_RECEIPT_DOMAIN_RECORD) &&
           receipt->opaque[1] == durable_gateway_assignment_receipt_binding(
               gateway_id, identity);
}

static int durable_encode_gateway_assignment(
    uint8_t record[APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE],
    uint64_t gateway_id,
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity,
    uint64_t retired_from_fingerprint)
{
    int ret;

    if (record == NULL || gateway_id == 0u || snapshot == NULL ||
        !durable_gateway_assignment_snapshot_identity_coherent(
            gateway_id, snapshot, identity)) {
        return -EINVAL;
    }
    memset(record, 0, APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE);
    durable_put_u32(&record[RECORD_MAGIC_OFFSET], APP_DURABLE_STATE_MAGIC);
    durable_put_u16(&record[RECORD_SCHEMA_OFFSET],
                    APP_DURABLE_STATE_ENVELOPE_VERSION);
    durable_put_u16(&record[RECORD_TOTAL_SIZE_OFFSET],
                    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE);
    durable_put_u16(&record[RECORD_TYPE_OFFSET],
                    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT);
    durable_put_u16(&record[RECORD_VERSION_OFFSET],
                    APP_DURABLE_STATE_RECORD_VERSION);
    record[RECORD_ROLE_OFFSET] = APP_DURABLE_STATE_ROLE_GATEWAY;
    record[RECORD_FLAGS_OFFSET] = APP_DURABLE_STATE_VALID_FLAG;
    durable_put_u64(&record[RECORD_DEVICE_ID_OFFSET], durable_owner.device_id);
    durable_put_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET],
                    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_PAYLOAD_SIZE);
    durable_put_u64(&record[GATEWAY_ASSIGNMENT_GATEWAY_ID_OFFSET], gateway_id);
    durable_put_u32(&record[GATEWAY_ASSIGNMENT_CORRELATION_ID_OFFSET],
                    identity->correlation_id);
    durable_put_u32(&record[GATEWAY_ASSIGNMENT_GATEWAY_SEQUENCE_OFFSET],
                    identity->gateway_sequence);
    durable_put_u32(&record[GATEWAY_ASSIGNMENT_HOST_SESSION_ID_OFFSET],
                    identity->host_session_id);
    durable_put_u16(&record[GATEWAY_ASSIGNMENT_GATEWAY_EPOCH_OFFSET],
                    identity->gateway_epoch);
    durable_put_u16(&record[GATEWAY_ASSIGNMENT_HOST_SEQ_OFFSET],
                    identity->host_seq);
    ret = gateway_membership_snapshot_encode(
        snapshot,
        &record[GATEWAY_ASSIGNMENT_SNAPSHOT_OFFSET],
        GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE);
    if (ret != PROTO_OK) {
        memset(record, 0, APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE);
        return -EPROTO;
    }
    if ((snapshot->publication_table_round != 0u) !=
        (retired_from_fingerprint == 0u)) {
        memset(record, 0, APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE);
        return -EINVAL;
    }
    durable_put_u64(&record[GATEWAY_ASSIGNMENT_RETIRED_FROM_FINGERPRINT_OFFSET],
                    retired_from_fingerprint);
    durable_put_u16(&record[RECORD_CRC_OFFSET],
                    durable_crc16(record,
                                  APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE));
    return 0;
}

static int durable_decode_gateway_assignment(
    uint8_t record[APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE],
    size_t record_len,
    uint64_t expected_gateway_id,
    struct gateway_membership_snapshot *snapshot,
    struct app_durable_state_gateway_assignment_identity *identity,
    bool *replay_debt,
    uint64_t *retired_from_fingerprint)
{
    uint16_t stored_crc;
    int ret;

    if (record == NULL || expected_gateway_id == 0u || snapshot == NULL ||
        identity == NULL || replay_debt == NULL ||
        retired_from_fingerprint == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    memset(identity, 0, sizeof(*identity));
    *replay_debt = false;
    *retired_from_fingerprint = 0u;
    if (record_len != APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE) {
        return -EILSEQ;
    }
    if (durable_get_u32(&record[RECORD_MAGIC_OFFSET]) !=
            APP_DURABLE_STATE_MAGIC ||
        durable_get_u16(&record[RECORD_SCHEMA_OFFSET]) !=
            APP_DURABLE_STATE_ENVELOPE_VERSION ||
        durable_get_u16(&record[RECORD_TOTAL_SIZE_OFFSET]) != record_len ||
        durable_get_u16(&record[RECORD_TYPE_OFFSET]) !=
            APP_DURABLE_STATE_GATEWAY_ASSIGNMENT ||
        durable_get_u16(&record[RECORD_VERSION_OFFSET]) !=
            APP_DURABLE_STATE_RECORD_VERSION ||
        record[RECORD_FLAGS_OFFSET] != APP_DURABLE_STATE_VALID_FLAG ||
        durable_get_u16(&record[RECORD_RESERVED16_OFFSET]) != 0u ||
        durable_get_u16(&record[RECORD_PAYLOAD_SIZE_OFFSET]) !=
            APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_PAYLOAD_SIZE ||
        durable_get_u32(&record[RECORD_RESERVED32_OFFSET]) != 0u) {
        return -EPROTO;
    }
    if (record[RECORD_ROLE_OFFSET] != APP_DURABLE_STATE_ROLE_GATEWAY ||
        durable_owner.role != APP_DURABLE_STATE_ROLE_GATEWAY ||
        durable_get_u64(&record[RECORD_DEVICE_ID_OFFSET]) !=
            durable_owner.device_id ||
        durable_get_u64(&record[GATEWAY_ASSIGNMENT_GATEWAY_ID_OFFSET]) !=
            expected_gateway_id) {
        return -EACCES;
    }
    stored_crc = durable_get_u16(&record[RECORD_CRC_OFFSET]);
    durable_put_u16(&record[RECORD_CRC_OFFSET], 0u);
    if (stored_crc != durable_crc16(
                          record,
                          APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE)) {
        return -EBADMSG;
    }
    durable_put_u16(&record[RECORD_CRC_OFFSET], stored_crc);

    identity->correlation_id = durable_get_u32(
        &record[GATEWAY_ASSIGNMENT_CORRELATION_ID_OFFSET]);
    identity->gateway_sequence = durable_get_u32(
        &record[GATEWAY_ASSIGNMENT_GATEWAY_SEQUENCE_OFFSET]);
    identity->host_session_id = durable_get_u32(
        &record[GATEWAY_ASSIGNMENT_HOST_SESSION_ID_OFFSET]);
    identity->gateway_epoch = durable_get_u16(
        &record[GATEWAY_ASSIGNMENT_GATEWAY_EPOCH_OFFSET]);
    identity->host_seq = durable_get_u16(
        &record[GATEWAY_ASSIGNMENT_HOST_SEQ_OFFSET]);
    if (!durable_gateway_assignment_identity_valid(identity)) {
        memset(identity, 0, sizeof(*identity));
        return -EPROTO;
    }
    ret = gateway_membership_snapshot_decode(
        &record[GATEWAY_ASSIGNMENT_SNAPSHOT_OFFSET],
        GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE,
        snapshot);
    if (ret != PROTO_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        return -EPROTO;
    }

    if (!durable_gateway_assignment_snapshot_identity_coherent(
            expected_gateway_id, snapshot, identity)) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        return -EPROTO;
    }

    *replay_debt = snapshot->publication_table_round != 0u;
    *retired_from_fingerprint = durable_get_u64(
        &record[GATEWAY_ASSIGNMENT_RETIRED_FROM_FINGERPRINT_OFFSET]);
    if (*replay_debt != (*retired_from_fingerprint == 0u)) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        *replay_debt = false;
        *retired_from_fingerprint = 0u;
        return -EPROTO;
    }
    if (*replay_debt &&
        (snapshot->publication_host_dst_id != expected_gateway_id ||
         snapshot->publication_host_session_id != identity->host_session_id ||
         snapshot->publication_host_seq != identity->host_seq ||
         snapshot->publication_event_gateway_epoch != identity->gateway_epoch)) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        *replay_debt = false;
        *retired_from_fingerprint = 0u;
        return -EPROTO;
    }
    return 0;
}

static int durable_read_gateway_assignment_locked(
    uint64_t gateway_id,
    struct gateway_membership_snapshot *snapshot,
    struct app_durable_state_gateway_assignment_identity *identity,
    bool *replay_debt,
    uint64_t *retired_from_fingerprint)
{
    ssize_t read_len;
    int ret;

    if (snapshot == NULL || identity == NULL || replay_debt == NULL ||
        retired_from_fingerprint == NULL) {
        return -EINVAL;
    }
    memset(durable_gateway_assignment_record,
           0,
           sizeof(durable_gateway_assignment_record));
    read_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT,
        durable_gateway_assignment_record,
        sizeof(durable_gateway_assignment_record));
    if (read_len == -ENOENT) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        *replay_debt = false;
        *retired_from_fingerprint = 0u;
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    ret = durable_decode_gateway_assignment(durable_gateway_assignment_record,
                                             (size_t)read_len,
                                             gateway_id,
                                             snapshot,
                                             identity,
                                             replay_debt,
                                             retired_from_fingerprint);
    return ret < 0 ? ret : 1;
}

static int durable_write_gateway_assignment_locked(
    uint64_t gateway_id,
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity,
    uint64_t retired_from_fingerprint,
    struct app_durable_receipt *receipt,
    bool *readback_unavailable)
{
    struct app_durable_state_gateway_assignment_identity verified_identity;
    uint64_t expected_fingerprint;
    uint64_t verified_retired_from_fingerprint;
    bool expected_replay_debt;
    bool verified_replay_debt;
    ssize_t io_len;
    int ret;

    if (readback_unavailable != NULL) {
        *readback_unavailable = false;
    }

    ret = durable_encode_gateway_assignment(durable_gateway_assignment_record,
                                            gateway_id,
                                            snapshot,
                                            identity,
                                            retired_from_fingerprint);
    if (ret < 0) {
        return ret;
    }
    expected_replay_debt = snapshot->publication_table_round != 0u;
    expected_fingerprint = durable_record_fingerprint(
        durable_gateway_assignment_record,
        sizeof(durable_gateway_assignment_record),
        DURABLE_RECEIPT_DOMAIN_RECORD);
    io_len = durable_owner.backend.write(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT,
        durable_gateway_assignment_record,
        sizeof(durable_gateway_assignment_record));
    if (io_len < 0) {
        return (int)io_len;
    }
    if (io_len != 0 && (size_t)io_len !=
                           sizeof(durable_gateway_assignment_record)) {
        return -EIO;
    }

    memset(durable_gateway_assignment_record,
           0,
           sizeof(durable_gateway_assignment_record));
    io_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT,
        durable_gateway_assignment_record,
        sizeof(durable_gateway_assignment_record));
    if (io_len < 0) {
        /* A missing entry proves this write did not become durable. Other
         * read errors leave an ambiguous write/readback boundary. */
        if (io_len != -ENOENT && readback_unavailable != NULL) {
            *readback_unavailable = true;
        }
        return io_len == -ENOENT ? -EIO : (int)io_len;
    }
    ret = durable_decode_gateway_assignment(
        durable_gateway_assignment_record,
        (size_t)io_len,
        gateway_id,
        &durable_gateway_assignment_snapshot,
        &verified_identity,
        &verified_replay_debt,
        &verified_retired_from_fingerprint);
    if (ret < 0) {
        return ret;
    }
    if (durable_record_fingerprint(durable_gateway_assignment_record,
                                   sizeof(durable_gateway_assignment_record),
                                   DURABLE_RECEIPT_DOMAIN_RECORD) !=
            expected_fingerprint ||
        verified_replay_debt != expected_replay_debt ||
        verified_retired_from_fingerprint != retired_from_fingerprint ||
        !durable_gateway_assignment_identities_equal(identity,
                                                      &verified_identity)) {
        return -EIO;
    }
    durable_gateway_assignment_issue_receipt(durable_gateway_assignment_record,
                                             gateway_id,
                                             &verified_identity,
                                             receipt);
    return 0;
}

static bool durable_gateway_assignment_terminal_matches(
    const struct app_durable_state_gateway_assignment_identity *identity,
    const struct gateway_command_event *event)
{
    return durable_gateway_assignment_identity_valid(identity) &&
           event != NULL &&
           event->schema_version == GATEWAY_COMMAND_EVENT_SCHEMA_VERSION &&
           event->record_len == GATEWAY_COMMAND_EVENT_WIRE_LEN &&
           event->kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION &&
           event->stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE &&
           (event->flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u &&
           event->command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           event->correlation_id == identity->correlation_id &&
           event->gateway_sequence == identity->gateway_sequence &&
           event->host_session_id == identity->host_session_id &&
           event->gateway_epoch == identity->gateway_epoch &&
           event->host_seq == identity->host_seq && event->event_seq != 0u &&
           event->anchor_id == 0u && event->pair_initiator_id == 0u &&
           event->pair_responder_id == 0u &&
           event->slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
}
#endif

static int durable_counter_next(const struct durable_counter_spec *spec,
                                uint64_t current,
                                uint64_t *next)
{
    uint64_t candidate;
    uint64_t limit = spec->counter_is_u32 ? UINT32_MAX : UINT64_MAX;

    if (next == NULL) {
        return -EINVAL;
    }
    *next = 0u;
    if (current > limit) {
        return -EILSEQ;
    }
    if (current == limit) {
        if (!spec->wraps_without_zero) {
            return -EOVERFLOW;
        }
        candidate = 1u;
    } else {
        candidate = current + 1u;
    }
    if (spec->skip_low_u32_zero && (uint32_t)candidate == 0u) {
        if (candidate == limit) {
            return -EOVERFLOW;
        }
        candidate++;
    }
    if (spec->skip_low_u16_zero && (uint16_t)candidate == 0u) {
        if (candidate == limit) {
            return -EOVERFLOW;
        }
        candidate++;
    }
    *next = candidate;
    return 0;
}

static int durable_counter_advance(
    const struct durable_counter_spec *spec,
    uint64_t current,
    uint32_t count,
    uint64_t *advanced)
{
    const uint64_t limit = spec != NULL && spec->counter_is_u32 ?
                           UINT32_MAX : UINT64_MAX;
    uint64_t normalized;

    if (spec == NULL || advanced == NULL || count == 0u) {
        return -EINVAL;
    }
    if (count == 1u) {
        return durable_counter_next(spec, current, advanced);
    }
    /* Multi-value blocks are deliberately plain contiguous counter domains. */
    if (spec->skip_low_u16_zero || spec->skip_low_u32_zero) {
        return -EINVAL;
    }
    if (spec->wraps_without_zero) {
        if (!spec->counter_is_u32 || current > limit) {
            return -EINVAL;
        }
        normalized = current == 0u ? 0u : current - 1u;
        *advanced = ((normalized + count) % limit) + 1u;
        return 0;
    }
    if (current > limit || count > limit || current > limit - count) {
        return -EOVERFLOW;
    }
    *advanced = current + count;
    return 0;
}

static int durable_reserve_locked(
    const struct durable_counter_spec *spec,
    uint64_t scope_id,
    struct app_durable_state_reservation *reservation)
{
    uint64_t previous;
    uint64_t first = 0u;
    int found;
    int ret;

    if (!durable_role_allowed(spec->reserve_roles)) {
        return -ENOTSUP;
    }
    found = durable_read_locked(spec, scope_id, &previous);
    if (found < 0) {
        return found;
    }
    if (found == 0) {
        previous = spec->first_install_floor;
    }
    ret = durable_counter_next(spec, previous, &first);
    if (ret < 0) {
        return ret;
    }
    ret = durable_counter_advance(spec,
                                  previous,
                                  spec->reservation_count,
                                  &previous);
    if (ret < 0) {
        return ret;
    }
    ret = durable_write_locked(spec, scope_id, previous);
    if (ret < 0) {
        return ret;
    }
    reservation->first = first;
    reservation->reserved_through = previous;
    return 0;
}

int app_durable_state_init(uint64_t device_id)
{
    const struct durable_counter_spec *boot_spec;
    uint64_t existing_boot = 0u;
    int ret;

    if (device_id == 0u) {
        return -EINVAL;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
#if !defined(APP_DURABLE_STATE_TESTING)
    durable_install_production_backend();
#endif
    if (!durable_owner.backend_installed ||
        durable_owner.backend.mount == NULL ||
        durable_owner.backend.read == NULL ||
        durable_owner.backend.write == NULL ||
        durable_owner.backend.erase == NULL ||
        durable_owner.role < APP_DURABLE_STATE_ROLE_CLICKER ||
        durable_owner.role > APP_DURABLE_STATE_ROLE_GATEWAY) {
        ret = -ENODEV;
        goto out;
    }
    if (durable_ready_get()) {
        ret = durable_owner.device_id == device_id ? 0 : -EACCES;
        goto out;
    }
    if (!durable_owner.mounted) {
        ret = durable_owner.backend.mount(durable_owner.backend.context);
        if (ret < 0) {
            goto out;
        }
        durable_owner.mounted = true;
    }
    durable_owner.device_id = device_id;
    durable_owner.boot_incarnation = 0u;
    boot_spec = durable_find_spec(APP_DURABLE_STATE_BOOT_INCARNATION);
    if (boot_spec == NULL) {
        ret = -ENODEV;
        goto out;
    }
    ret = durable_read_locked(boot_spec, 0u, &existing_boot);
    if (ret < 0) {
        goto out;
    }
    durable_ready_set(true);
    ret = 0;
out:
    durable_unlock();
    return ret;
}

int app_durable_state_begin_boot(void)
{
    const struct durable_counter_spec *boot_spec;
    struct app_durable_state_reservation boot = {0};
    int ret;

    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get()) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.boot_incarnation != 0u) {
        ret = 0;
        goto out;
    }
    boot_spec = durable_find_spec(APP_DURABLE_STATE_BOOT_INCARNATION);
    if (boot_spec == NULL) {
        ret = -ENODEV;
        goto out;
    }
    ret = durable_reserve_locked(boot_spec, 0u, &boot);
    if (ret < 0) {
        goto out;
    }
    if (boot.first != boot.reserved_through ||
        boot.first == 0u || boot.first > UINT32_MAX ||
        (uint16_t)boot.first == 0u) {
        ret = -EILSEQ;
        goto out;
    }
    durable_owner.boot_incarnation = (uint32_t)boot.first;
    ret = 0;
out:
    durable_unlock();
    return ret;
}

bool app_durable_state_ready(void)
{
    return durable_ready_get();
}

int app_durable_state_boot_incarnation(uint32_t *incarnation)
{
    int ret;

    if (incarnation == NULL) {
        return -EINVAL;
    }
    *incarnation = 0u;
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
    } else {
        *incarnation = durable_owner.boot_incarnation;
        ret = 0;
    }
    durable_unlock();
    return ret;
}

int app_durable_state_reserve(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    struct app_durable_state_reservation *reservation)
{
    const struct durable_counter_spec *spec = durable_find_spec(type);
    int ret;

    if (reservation == NULL || spec == NULL) {
        return -EINVAL;
    }
    memset(reservation, 0, sizeof(*reservation));
    if (type == APP_DURABLE_STATE_BOOT_INCARNATION) {
        return -ENOTSUP;
    }
    ret = durable_validate_scope(spec, scope_id);
    if (ret < 0) {
        return ret;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    ret = durable_reserve_locked(spec, scope_id, reservation);
out:
    durable_unlock();
    return ret;
}

int app_durable_state_restore_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t *reserved_through)
{
    const struct durable_counter_spec *spec = durable_find_spec(type);
    int ret;

    if (reserved_through == NULL || spec == NULL) {
        return -EINVAL;
    }
    *reserved_through = 0u;
    ret = durable_validate_scope(spec, scope_id);
    if (ret < 0) {
        return ret;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get()) {
        ret = -EACCES;
        goto out;
    }
    if (!durable_role_allowed(spec->restore_roles)) {
        ret = -ENOTSUP;
        goto out;
    }
    ret = durable_read_locked(spec, scope_id, reserved_through);
out:
    durable_unlock();
    return ret;
}

int app_durable_state_advance_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t candidate)
{
    const struct durable_counter_spec *spec = durable_find_spec(type);
    uint64_t previous;
    int found;
    int ret;

    if (spec == NULL) {
        return -EINVAL;
    }
    ret = durable_validate_scope(spec, scope_id);
    if (ret < 0) {
        return ret;
    }
    ret = durable_validate_high_water(spec, candidate);
    if (ret < 0) {
        return ret == -EILSEQ ? -EINVAL : ret;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (!durable_role_allowed(spec->advance_roles)) {
        ret = -ENOTSUP;
        goto out;
    }
    found = durable_read_locked(spec, scope_id, &previous);
    if (found < 0) {
        ret = found;
        goto out;
    }
    if (found == 1 && candidate < previous) {
        ret = -ESTALE;
        goto out;
    }
    if (found == 1 && candidate == previous) {
        ret = 0;
        goto out;
    }
    ret = durable_write_locked(spec, scope_id, candidate);
out:
    durable_unlock();
    return ret;
}

int app_durable_state_save_anchor_assignment(
    uint64_t gateway_id,
    const struct app_durable_state_anchor_assignment *assignment)
{
    uint8_t record[APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE];
    struct app_durable_state_anchor_assignment verified;
    ssize_t io_len;
    int ret;

    if (gateway_id == 0u || assignment == NULL) {
        return -EINVAL;
    }
    ret = durable_validate_anchor_assignment(assignment);
    if (ret < 0) {
        return ret;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_ANCHOR) {
        ret = -ENOTSUP;
        goto out;
    }

    memset(record, 0, sizeof(record));
    io_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (io_len != -ENOENT) {
        if (io_len < 0) {
            ret = (int)io_len;
            goto out;
        }
        ret = durable_decode_anchor_assignment(record,
                                                (size_t)io_len,
                                                gateway_id,
                                                &verified);
        if (ret < 0) {
            goto out;
        }
        if (durable_anchor_assignments_equal(assignment, &verified)) {
            ret = 0;
            goto out;
        }
    }

    durable_encode_anchor_assignment(record, gateway_id, assignment);
    io_len = durable_owner.backend.write(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (io_len < 0) {
        ret = (int)io_len;
        goto out;
    }
    if (io_len != 0 && (size_t)io_len != sizeof(record)) {
        ret = -EIO;
        goto out;
    }

    memset(record, 0, sizeof(record));
    io_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (io_len < 0) {
        ret = io_len == -ENOENT ? -EIO : (int)io_len;
        goto out;
    }
    ret = durable_decode_anchor_assignment(record,
                                            (size_t)io_len,
                                            gateway_id,
                                            &verified);
    if (ret == 0 && !durable_anchor_assignments_equal(assignment,
                                                      &verified)) {
        ret = -EIO;
    }
out:
    durable_unlock();
    return ret;
}

int app_durable_state_restore_anchor_assignment(
    uint64_t gateway_id,
    struct app_durable_state_anchor_assignment *assignment)
{
    uint8_t record[APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE];
    ssize_t read_len;
    int ret;

    if (gateway_id == 0u || assignment == NULL) {
        return -EINVAL;
    }
    memset(assignment, 0, sizeof(*assignment));
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get()) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_ANCHOR) {
        ret = -ENOTSUP;
        goto out;
    }
    memset(record, 0, sizeof(record));
    read_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (read_len == -ENOENT) {
        ret = 0;
        goto out;
    }
    if (read_len < 0) {
        ret = (int)read_len;
        goto out;
    }
    ret = durable_decode_anchor_assignment(record,
                                            (size_t)read_len,
                                            gateway_id,
                                            assignment);
    if (ret == 0) {
        ret = 1;
    }
out:
    if (ret <= 0) {
        memset(assignment, 0, sizeof(*assignment));
    }
    durable_unlock();
    return ret;
}

int app_durable_state_delete_anchor_assignment(uint64_t gateway_id)
{
    uint8_t record[APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE];
    struct app_durable_state_anchor_assignment existing;
    ssize_t read_len;
    int ret;

    if (gateway_id == 0u) {
        return -EINVAL;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_ANCHOR) {
        ret = -ENOTSUP;
        goto out;
    }
    memset(record, 0, sizeof(record));
    read_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (read_len == -ENOENT) {
        ret = 0;
        goto out;
    }
    if (read_len < 0) {
        ret = (int)read_len;
        goto out;
    }
    ret = durable_decode_anchor_assignment(record,
                                            (size_t)read_len,
                                            gateway_id,
                                            &existing);
    if (ret < 0) {
        goto out;
    }
    ret = durable_owner.backend.erase(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT);
    if (ret < 0 && ret != -ENOENT) {
        goto out;
    }
    if (ret > 0) {
        ret = -EIO;
        goto out;
    }
    memset(record, 0, sizeof(record));
    read_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT,
        record,
        sizeof(record));
    if (read_len == -ENOENT) {
        ret = 0;
    } else if (read_len < 0) {
        ret = (int)read_len;
    } else {
        ret = -EIO;
    }
out:
    durable_unlock();
    return ret;
}

int app_durable_state_save_gateway_assignment_commit(
    uint64_t gateway_id,
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity,
    struct app_durable_receipt *receipt)
{
#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
    struct app_durable_state_gateway_assignment_identity existing_identity;
    uint64_t existing_retired_from_fingerprint = 0u;
    bool existing_replay_debt;
    bool readback_unavailable = false;
    int found;
    int ret;

    if (gateway_id == 0u || snapshot == NULL || identity == NULL ||
        receipt == NULL) {
        return -EINVAL;
    }
    durable_receipt_clear(receipt);
    if (!durable_gateway_assignment_snapshot_identity_coherent(
            gateway_id, snapshot, identity) ||
        snapshot->publication_table_round == 0u) {
        return -EINVAL;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_GATEWAY) {
        ret = -ENOTSUP;
        goto out;
    }

    found = durable_read_gateway_assignment_locked(
        gateway_id,
        &durable_gateway_assignment_snapshot,
        &existing_identity,
        &existing_replay_debt,
        &existing_retired_from_fingerprint);
    (void)existing_retired_from_fingerprint;
    if (found < 0) {
        ret = found;
        goto out;
    }
    if (found == 1) {
        if (existing_replay_debt) {
            if (durable_gateway_assignment_identities_equal(
                    identity, &existing_identity) &&
                gateway_membership_snapshot_semantically_equal(
                    snapshot, &durable_gateway_assignment_snapshot)) {
                durable_gateway_assignment_issue_receipt(
                    durable_gateway_assignment_record,
                    gateway_id,
                    &existing_identity,
                    receipt);
                ret = 0;
            } else {
                ret = -EBUSY;
            }
            goto out;
        }
        if (durable_gateway_assignment_identities_equal(identity,
                                                        &existing_identity)) {
            ret = -ESTALE;
            goto out;
        }
    }
    ret = durable_write_gateway_assignment_locked(gateway_id,
                                                   snapshot,
                                                   identity,
                                                   0u,
                                                   receipt,
                                                   &readback_unavailable);
    if (ret < 0 && readback_unavailable) {
        /* The exact record may be durable, but a capability is only minted
         * after an exact retry reads it back. */
        ret = APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_SAVE_ADOPT_REQUIRED;
    }
out:
    if (ret < 0) {
        durable_receipt_clear(receipt);
    }
    durable_unlock();
    return ret;
#else
    (void)gateway_id;
    (void)snapshot;
    (void)identity;
    durable_receipt_clear(receipt);
    return -ENOTSUP;
#endif
}

int app_durable_state_restore_gateway_assignment_commit(
    uint64_t gateway_id,
    struct gateway_membership_snapshot *snapshot,
    struct app_durable_state_gateway_assignment_identity *identity,
    bool *replay_debt,
    struct app_durable_receipt *receipt)
{
#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
    uint64_t retired_from_fingerprint = 0u;
    int found;
    int ret;

    if (gateway_id == 0u || snapshot == NULL || identity == NULL ||
        replay_debt == NULL || receipt == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    memset(identity, 0, sizeof(*identity));
    *replay_debt = false;
    durable_receipt_clear(receipt);
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get()) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_GATEWAY) {
        ret = -ENOTSUP;
        goto out;
    }
    found = durable_read_gateway_assignment_locked(gateway_id,
                                                   snapshot,
                                                   identity,
                                                   replay_debt,
                                                   &retired_from_fingerprint);
    (void)retired_from_fingerprint;
    if (found == 1) {
        durable_gateway_assignment_issue_receipt(durable_gateway_assignment_record,
                                                 gateway_id,
                                                 identity,
                                                 receipt);
        ret = 1;
    } else {
        ret = found;
    }
out:
    if (ret != 1) {
        memset(snapshot, 0, sizeof(*snapshot));
        memset(identity, 0, sizeof(*identity));
        *replay_debt = false;
        durable_receipt_clear(receipt);
    }
    durable_unlock();
    return ret;
#else
    (void)gateway_id;
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    if (identity != NULL) {
        memset(identity, 0, sizeof(*identity));
    }
    if (replay_debt != NULL) {
        *replay_debt = false;
    }
    durable_receipt_clear(receipt);
    return -ENOTSUP;
#endif
}

int app_durable_state_retire_gateway_assignment_commit(
    uint64_t gateway_id,
    const struct app_durable_receipt *receipt,
    const struct gateway_command_event *terminal_event,
    struct app_durable_receipt *retired_receipt)
{
#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
    struct app_durable_state_gateway_assignment_identity identity;
    uint64_t retired_from_fingerprint = 0u;
    bool replay_debt;
    int found;
    int ret;

    if (gateway_id == 0u || receipt == NULL || terminal_event == NULL ||
        retired_receipt == NULL) {
        return -EINVAL;
    }
    durable_receipt_clear(retired_receipt);
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_GATEWAY) {
        ret = -ENOTSUP;
        goto out;
    }
    found = durable_read_gateway_assignment_locked(
        gateway_id,
        &durable_gateway_assignment_snapshot,
        &identity,
        &replay_debt,
        &retired_from_fingerprint);
    if (found < 0) {
        ret = found;
        goto out;
    }
    if (found == 0 ||
        !durable_gateway_assignment_terminal_matches(&identity,
                                                     terminal_event)) {
        ret = -ESTALE;
        goto out;
    }
    if (!replay_debt) {
        /*
         * The prior write may have committed before its readback failed.
         * Require the exact predecessor receipt stored by that transition
         * before returning the new retired receipt without another NVS write.
         */
        if (receipt->opaque[0] != retired_from_fingerprint ||
            receipt->opaque[1] !=
                durable_gateway_assignment_receipt_binding(gateway_id,
                                                            &identity)) {
            ret = -ESTALE;
            goto out;
        }
        durable_gateway_assignment_issue_receipt(
            durable_gateway_assignment_record,
            gateway_id,
            &identity,
            retired_receipt);
        ret = 0;
        goto out;
    }
    if (!durable_gateway_assignment_receipt_matches(
            durable_gateway_assignment_record,
            gateway_id,
            &identity,
            receipt)) {
        ret = -ESTALE;
        goto out;
    }
    ret = gateway_membership_snapshot_retire_publication(
        &durable_gateway_assignment_snapshot,
        &durable_gateway_assignment_snapshot);
    if (ret != PROTO_OK) {
        ret = -EPROTO;
        goto out;
    }
    ret = durable_write_gateway_assignment_locked(
        gateway_id,
        &durable_gateway_assignment_snapshot,
        &identity,
        durable_record_fingerprint(
            durable_gateway_assignment_record,
            sizeof(durable_gateway_assignment_record),
            DURABLE_RECEIPT_DOMAIN_RECORD),
        retired_receipt,
        NULL);
out:
    if (ret < 0) {
        durable_receipt_clear(retired_receipt);
    }
    durable_unlock();
    return ret;
#else
    (void)gateway_id;
    (void)receipt;
    (void)terminal_event;
    durable_receipt_clear(retired_receipt);
    return -ENOTSUP;
#endif
}

int app_durable_state_delete_gateway_assignment(
    uint64_t gateway_id,
    const struct app_durable_state_gateway_assignment_identity *identity,
    const struct app_durable_receipt *receipt)
{
#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
    struct app_durable_state_gateway_assignment_identity stored_identity;
    uint64_t retired_from_fingerprint;
    bool replay_debt;
    ssize_t read_len;
    int found;
    int ret;

    if (gateway_id == 0u || identity == NULL || receipt == NULL ||
        !durable_gateway_assignment_identity_valid(identity)) {
        return -EINVAL;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get() || durable_owner.boot_incarnation == 0u) {
        ret = -EACCES;
        goto out;
    }
    if (durable_owner.role != APP_DURABLE_STATE_ROLE_GATEWAY) {
        ret = -ENOTSUP;
        goto out;
    }
    found = durable_read_gateway_assignment_locked(
        gateway_id,
        &durable_gateway_assignment_snapshot,
        &stored_identity,
        &replay_debt,
        &retired_from_fingerprint);
    if (found < 0) {
        ret = found;
        goto out;
    }
    (void)retired_from_fingerprint;
    if (found == 0) {
        /* An erase may have landed before readback failed. No live record can
         * be removed on this retry, but require the exact scope/operation
         * binding instead of treating a zero or cross-operation receipt as a
         * successful reset. */
        ret = receipt->opaque[0] != 0u &&
                      receipt->opaque[1] ==
                          durable_gateway_assignment_receipt_binding(
                              gateway_id, identity) ?
                  0 :
                  -ESTALE;
        goto out;
    }
    if (!durable_gateway_assignment_identities_equal(identity,
                                                      &stored_identity) ||
        !durable_gateway_assignment_receipt_matches(
            durable_gateway_assignment_record,
            gateway_id,
            &stored_identity,
            receipt)) {
        ret = -ESTALE;
        goto out;
    }
    /* Decommission is valid only after the host terminal receipt retired the
     * immutable publication debt. A pending record remains the sole source
     * of reset replay and must never be erasable through this typed API. */
    if (replay_debt) {
        ret = -EBUSY;
        goto out;
    }
    ret = durable_owner.backend.erase(durable_owner.backend.context,
                                      APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT);
    if (ret < 0 && ret != -ENOENT) {
        goto out;
    }
    if (ret > 0) {
        ret = -EIO;
        goto out;
    }
    memset(durable_gateway_assignment_record,
           0,
           sizeof(durable_gateway_assignment_record));
    read_len = durable_owner.backend.read(
        durable_owner.backend.context,
        APP_DURABLE_STATE_ID_GATEWAY_ASSIGNMENT,
        durable_gateway_assignment_record,
        sizeof(durable_gateway_assignment_record));
    if (read_len == -ENOENT) {
        ret = 0;
    } else if (read_len < 0) {
        ret = (int)read_len;
    } else {
        ret = -EIO;
    }
out:
    durable_unlock();
    return ret;
#else
    (void)gateway_id;
    (void)identity;
    (void)receipt;
    return -ENOTSUP;
#endif
}

#if defined(APP_DURABLE_STATE_TESTING)
int app_durable_state_test_install_backend(
    const struct app_durable_state_test_backend *backend,
    enum app_durable_state_role role)
{
    if (backend == NULL || backend->mount == NULL || backend->read == NULL ||
        backend->write == NULL || backend->erase == NULL ||
        role < APP_DURABLE_STATE_ROLE_CLICKER ||
        role > APP_DURABLE_STATE_ROLE_GATEWAY || durable_ready_get()) {
        return -EINVAL;
    }
    durable_owner.backend = (struct durable_backend) {
        .context = backend->context,
        .mount = backend->mount,
        .read = backend->read,
        .write = backend->write,
        .erase = backend->erase,
    };
    durable_owner.role = role;
    durable_owner.backend_installed = true;
    return 0;
}

void app_durable_state_test_reset(void)
{
    memset(&durable_owner, 0, sizeof(durable_owner));
#if DURABLE_GATEWAY_ASSIGNMENT_ENABLED
    memset(durable_gateway_assignment_record,
           0,
           sizeof(durable_gateway_assignment_record));
    memset(&durable_gateway_assignment_snapshot,
           0,
           sizeof(durable_gateway_assignment_snapshot));
#endif
    durable_ready_set(false);
}

int app_durable_state_test_seed_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t reserved_through)
{
    const struct durable_counter_spec *spec = durable_find_spec(type);
    int ret;

    if (spec == NULL) {
        return -EINVAL;
    }
    ret = durable_validate_scope(spec, scope_id);
    if (ret < 0) {
        return ret;
    }
    ret = durable_lock();
    if (ret < 0) {
        return ret;
    }
    if (!durable_ready_get()) {
        ret = -EACCES;
        goto out;
    }
    if (!durable_role_allowed(spec->restore_roles)) {
        ret = -ENOTSUP;
        goto out;
    }
    ret = durable_write_locked(spec, scope_id, reserved_through);
out:
    durable_unlock();
    return ret;
}
#endif
