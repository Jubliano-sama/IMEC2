#include "app_click_event_sequence.h"
#include "app_nvs_storage.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE)
#define click_event_nvs (*app_nvs_storage_fs())
#endif

LOG_MODULE_REGISTER(app_click_event_sequence, LOG_LEVEL_INF);

#define CLICK_EVENT_SEQUENCE_RECORD_MAGIC UINT32_C(0x43455351)
#define CLICK_EVENT_SEQUENCE_RECORD_VERSION 1u

static K_MUTEX_DEFINE(click_event_sequence_lock);
static uint32_t click_event_next;
static uint32_t click_event_reserved_through;
static bool click_event_sequence_ready;

#if defined(CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE)
struct click_event_sequence_record {
    uint32_t magic;
    uint32_t reserved_through;
    uint16_t version;
    uint16_t block_size;
};

BUILD_ASSERT(IS_ENABLED(CONFIG_NVS_DATA_CRC),
             "durable click identities require NVS data CRC");
BUILD_ASSERT(APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE <= UINT16_MAX,
             "click event sequence block size must fit its persistent record");
BUILD_ASSERT((APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR %
              APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE) == 0u,
             "click event first-install floor must be block aligned");
BUILD_ASSERT(APP_CLICK_EVENT_SEQUENCE_MAX_BOOT_RESERVATIONS == 16711679u,
             "documented durable click sequence boot bound changed");

static int click_event_sequence_validate_record(
    const struct click_event_sequence_record *record)
{
    if (record == NULL ||
        record->magic != CLICK_EVENT_SEQUENCE_RECORD_MAGIC ||
        record->version != CLICK_EVENT_SEQUENCE_RECORD_VERSION ||
        record->block_size != APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE ||
        record->reserved_through == 0u ||
        (record->reserved_through % APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE) != 0u) {
        return -EILSEQ;
    }
    return 0;
}

static int click_event_sequence_read_record(uint32_t *reserved_through,
                                            bool *present)
{
    struct click_event_sequence_record record;
    ssize_t read_len;
    int ret;

    if (reserved_through == NULL || present == NULL) {
        return -EINVAL;
    }
    *reserved_through = 0u;
    *present = false;

    read_len = nvs_read(&click_event_nvs,
                        APP_NVS_ID_CLICK_EVENT_SEQUENCE,
                        &record,
                        sizeof(record));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if (read_len != sizeof(record)) {
        return -EILSEQ;
    }

    ret = click_event_sequence_validate_record(&record);
    if (ret < 0) {
        return ret;
    }
    *reserved_through = record.reserved_through;
    *present = true;
    return 0;
}

static int click_event_sequence_reserve_block(uint32_t previous_limit,
                                              uint32_t *reserved_through)
{
    struct click_event_sequence_record verify_record;
    struct click_event_sequence_record record = {
        .magic = CLICK_EVENT_SEQUENCE_RECORD_MAGIC,
        .version = CLICK_EVENT_SEQUENCE_RECORD_VERSION,
        .block_size = APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE,
    };
    ssize_t io_len;
    int ret;

    if (reserved_through == NULL) {
        return -EINVAL;
    }
    if (previous_limit >
        UINT32_MAX - APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE) {
        return -EOVERFLOW;
    }
    record.reserved_through =
        previous_limit + APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE;

    /*
     * The durable high watermark moves before any ID in its block is exposed.
     * NVS keeps the preceding valid entry across an interrupted write, and a
     * read-back check prevents a reported-but-unreadable write from being
     * treated as a reservation.
     */
    io_len = nvs_write(&click_event_nvs,
                       APP_NVS_ID_CLICK_EVENT_SEQUENCE,
                       &record,
                       sizeof(record));
    if (io_len < 0) {
        return (int)io_len;
    }
    if (io_len != 0 && io_len != sizeof(record)) {
        return -EIO;
    }

    io_len = nvs_read(&click_event_nvs,
                      APP_NVS_ID_CLICK_EVENT_SEQUENCE,
                      &verify_record,
                      sizeof(verify_record));
    if (io_len < 0) {
        return (int)io_len;
    }
    if (io_len != sizeof(verify_record)) {
        return -EIO;
    }
    ret = click_event_sequence_validate_record(&verify_record);
    if (ret < 0 ||
        verify_record.reserved_through != record.reserved_through) {
        return ret < 0 ? ret : -EIO;
    }

    *reserved_through = record.reserved_through;
    return 0;
}

static int click_event_sequence_mount(void)
{
    return app_nvs_storage_init();
}
#endif

int app_click_event_sequence_init(void)
{
    int ret = 0;

    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (click_event_sequence_ready) {
        k_mutex_unlock(&click_event_sequence_lock);
        return 0;
    }

#if defined(CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE)
    {
        uint32_t previous_limit;
        bool present;

        ret = click_event_sequence_mount();
        if (ret < 0) {
            goto out;
        }
        ret = click_event_sequence_read_record(&previous_limit, &present);
        if (ret < 0) {
            goto out;
        }
        if (!present) {
            /*
             * Pre-persistence production System-OFF firmware restarted its
             * volatile counter from one on every click. Start a fresh install
             * above that realistic legacy range so an upgrade cannot collide
             * with gateway history still retained from the old image.
             */
            previous_limit =
                APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR;
        }
        ret = click_event_sequence_reserve_block(
            previous_limit, &click_event_reserved_through);
        if (ret < 0) {
            goto out;
        }
        click_event_next = previous_limit + 1u;
        LOG_INF("reserved durable click event IDs %u..%u",
                click_event_next,
                click_event_reserved_through);
    }
#else
    click_event_next = 1u;
    click_event_reserved_through = UINT32_MAX;
#endif
    click_event_sequence_ready = true;

#if defined(CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE)
out:
#endif
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}

int app_click_event_sequence_next(uint32_t *event_seq)
{
    int ret = 0;

    if (event_seq == NULL) {
        return -EINVAL;
    }
    *event_seq = 0u;

    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (!click_event_sequence_ready) {
        ret = -EACCES;
        goto out;
    }
    if (click_event_next == 0u) {
        ret = -EOVERFLOW;
        goto out;
    }

#if defined(CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE)
    if (click_event_next > click_event_reserved_through) {
        ret = click_event_sequence_reserve_block(
            click_event_reserved_through, &click_event_reserved_through);
        if (ret < 0) {
            goto out;
        }
    }
#endif

    *event_seq = click_event_next;
    click_event_next++;

out:
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}
