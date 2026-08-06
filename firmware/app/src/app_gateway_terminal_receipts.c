#include "app_gateway_terminal_receipts.h"

#include "app_config.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_gateway_terminal_receipts, LOG_LEVEL_INF);

struct __packed app_gateway_terminal_receipt_record {
    uint64_t src_id;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t valid;
};

BUILD_ASSERT(sizeof(struct app_gateway_terminal_receipt_record) ==
                 APP_GATEWAY_TERMINAL_RECEIPT_RECORD_SIZE,
             "gateway terminal receipt schema size changed");
BUILD_ASSERT(APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY ==
                 MESH_CONNECTED_REQUIRED_SOURCES,
             "terminal receipts must cover anchors and clickers");
BUILD_ASSERT(APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS <= INT32_MAX,
             "terminal receipt horizon must remain wrap-safe");
BUILD_ASSERT(APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS ==
                 COMMAND_RESULT_EXPIRY_DEFAULT_S * 1000u,
             "gateway and source raw-custody horizons must match");

K_MUTEX_DEFINE(gateway_terminal_receipt_mutex);
static uint32_t gateway_terminal_receipt_observed_at_ms[
    APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY];
static bool gateway_terminal_receipts_initialized;

/*
 * RAM-only fixed-slot receipt store replacing the former per-slot NVS
 * terminal receipts. Nothing is restored from flash and every write lasts
 * only for the current session; the encoded record mirrors the previous
 * durable schema so classify/confirm/record semantics are unchanged in RAM.
 */
static struct app_gateway_terminal_receipt_record
    gateway_terminal_receipt_record_store[APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY];

BUILD_ASSERT(sizeof(gateway_terminal_receipt_observed_at_ms) <= 512u,
             "terminal receipt runtime index must remain compact");

static bool terminal_receipt_record_valid(
    const struct app_gateway_terminal_receipt_record *record)
{
    return record != NULL &&
           record->valid == 1u &&
           record->src_id != 0u;
}

/*
 * RAM-only equivalent of the former persistence host-journal support check.
 * It is a pure packet classifier (no storage) and gates which packets take
 * the terminal-receipt path; persistence "restore" restored nothing and the
 * "save" is a no-op, so only the in-session cache is consulted.
 */
static bool terminal_receipt_host_journal_supported(
    const struct proto_packet *packet)
{
    if (packet == NULL ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    case MSG_MESH_DATA:
        return (packet->flags & FLAG_DIAGNOSTIC) != 0u;
    default:
        return false;
    }
}

static bool terminal_receipt_packet_supported(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *collection_epoch = NULL;
    uint8_t collection_epoch_len = 0u;

    if (!terminal_receipt_host_journal_supported(packet) ||
        packet->msg_type == MSG_RESULT_BUNDLE) {
        return false;
    }
    if (packet->msg_type != MSG_COMMAND_RESULT) {
        return true;
    }
    /*
     * Collection results retain their existing per-node durable receipts and
     * EACK protocol. Only ordinary command results use ACK_CONFIRM.
     */
    return tlv_find_unique(payload,
                           payload_len,
                           TLV_COLLECTION_EPOCH_ID,
                           &collection_epoch,
                           &collection_epoch_len) != PROTO_OK;
}

bool app_gateway_terminal_receipts_supports(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return packet != NULL &&
           packet->payload_len == payload_len &&
           (payload != NULL || payload_len == 0u) &&
           terminal_receipt_packet_supported(packet, payload, payload_len);
}

static int terminal_receipt_packet_digest(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (packet == NULL ||
        (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        !app_gateway_terminal_receipts_supports(
            packet, payload, payload_len) ||
        !mesh_packet_semantic_digest(packet, payload, payload_len, digest)) {
        return -EINVAL;
    }
    return 0;
}

static bool terminal_receipt_expired(uint8_t slot, uint32_t now_ms)
{
    return gateway_terminal_receipt_observed_at_ms[slot] != 0u &&
           (uint32_t)(now_ms -
                      gateway_terminal_receipt_observed_at_ms[slot]) >
               APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS;
}

static int terminal_receipt_read(
    uint8_t slot,
    struct app_gateway_terminal_receipt_record *record)
{
    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY) {
        return -EINVAL;
    }
    if (!terminal_receipt_record_valid(
            &gateway_terminal_receipt_record_store[slot])) {
        return 0;
    }
    *record = gateway_terminal_receipt_record_store[slot];
    return 1;
}

static int terminal_receipts_restore_locked(uint32_t now_ms)
{
    ARG_UNUSED(now_ms);

    if (gateway_terminal_receipts_initialized) {
        return 0;
    }
    /*
     * Nothing is restored from flash: a fresh session starts with an empty
     * cache. Persistence restore is a no-op, so just clear the RAM store.
     */
    memset(gateway_terminal_receipt_record_store,
           0,
           sizeof(gateway_terminal_receipt_record_store));
    memset(gateway_terminal_receipt_observed_at_ms,
           0,
           sizeof(gateway_terminal_receipt_observed_at_ms));
    gateway_terminal_receipts_initialized = true;
    return 0;
}

int app_gateway_terminal_receipts_restore(uint32_t now_ms)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    k_mutex_lock(&gateway_terminal_receipt_mutex, K_FOREVER);
    ret = terminal_receipts_restore_locked(now_ms);
    k_mutex_unlock(&gateway_terminal_receipt_mutex);
    return ret;
}

static int terminal_receipt_expire_slot_locked(uint8_t slot)
{
    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY) {
        return -EINVAL;
    }
    memset(&gateway_terminal_receipt_record_store[slot],
           0,
           sizeof(gateway_terminal_receipt_record_store[slot]));
    gateway_terminal_receipt_observed_at_ms[slot] = 0u;
    return 0;
}

static int terminal_receipts_classify_identity_locked(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t now_ms)
{
    bool source_match = false;
    int ret;

    ret = terminal_receipts_restore_locked(now_ms);
    if (ret < 0) {
        return ret;
    }
    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY;
         slot++) {
        struct app_gateway_terminal_receipt_record record;

        ret = terminal_receipt_read(slot, &record);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            continue;
        }
        if (terminal_receipt_expired(slot, now_ms)) {
            ret = terminal_receipt_expire_slot_locked(slot);
            if (ret < 0) {
                return ret;
            }
            continue;
        }
        if (record.src_id != packet->src_id) {
            continue;
        }
        if (source_match ||
            !semantic_digest_equal(record.semantic_digest,
                                   semantic_digest,
                                   SEMANTIC_DIGEST_SHA256_LEN)) {
            return -EBADMSG;
        }
        source_match = true;
    }
    return source_match ? 1 : 0;
}

int app_gateway_terminal_receipts_classify_identity(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t now_ms)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (packet == NULL || packet->src_id == 0u ||
        semantic_digest == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&gateway_terminal_receipt_mutex, K_FOREVER);
    ret = terminal_receipts_classify_identity_locked(
        packet, semantic_digest, now_ms);
    k_mutex_unlock(&gateway_terminal_receipt_mutex);
    return ret;
}

int app_gateway_terminal_receipts_classify(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    ret = terminal_receipt_packet_digest(
        packet, payload, payload_len, semantic_digest);
    if (ret < 0) {
        return ret;
    }
    return app_gateway_terminal_receipts_classify_identity(
        packet, semantic_digest, now_ms);
}

int app_gateway_terminal_receipts_record(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    struct app_gateway_terminal_receipt_record new_record = {0};
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool source_match = false;
    int free_slot = -1;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    ret = terminal_receipt_packet_digest(
        packet, payload, payload_len, semantic_digest);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&gateway_terminal_receipt_mutex, K_FOREVER);
    ret = terminal_receipts_restore_locked(now_ms);
    if (ret < 0) {
        goto out;
    }
    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY;
         slot++) {
        struct app_gateway_terminal_receipt_record record;

        ret = terminal_receipt_read(slot, &record);
        if (ret < 0) {
            goto out;
        }
        if (ret == 0) {
            if (free_slot < 0) {
                free_slot = slot;
            }
            continue;
        }
        if (terminal_receipt_expired(slot, now_ms)) {
            ret = terminal_receipt_expire_slot_locked(slot);
            if (ret < 0) {
                goto out;
            }
            if (free_slot < 0) {
                free_slot = slot;
            }
            continue;
        }
        if (record.src_id != packet->src_id) {
            continue;
        }
        if (source_match ||
            !semantic_digest_equal(record.semantic_digest,
                                   semantic_digest,
                                   sizeof(semantic_digest))) {
            ret = -EBADMSG;
            goto out;
        }
        source_match = true;
    }
    if (source_match) {
        ret = 0;
        goto out;
    }
    if (free_slot < 0) {
        ret = -ENOSPC;
        goto out;
    }

    new_record.src_id = packet->src_id;
    memcpy(new_record.semantic_digest,
           semantic_digest,
           sizeof(new_record.semantic_digest));
    new_record.valid = 1u;
    gateway_terminal_receipt_record_store[free_slot] = new_record;
    gateway_terminal_receipt_observed_at_ms[free_slot] =
        now_ms == 0u ? 1u : now_ms;
    ret = 1;

out:
    k_mutex_unlock(&gateway_terminal_receipt_mutex);
    return ret;
}

int app_gateway_terminal_receipts_confirm(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint32_t now_ms)
{
    struct proto_packet original_packet;
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int matched_slot = -1;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (mesh_gateway_ack_confirm_identity_packet(
            confirm_packet,
            confirm_payload,
            confirm_payload_len,
            &original_packet,
            original_digest) != PROTO_OK) {
        return -EINVAL;
    }

    k_mutex_lock(&gateway_terminal_receipt_mutex, K_FOREVER);
    ret = terminal_receipts_restore_locked(now_ms);
    if (ret < 0) {
        goto out;
    }
    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY;
         slot++) {
        struct app_gateway_terminal_receipt_record record;

        ret = terminal_receipt_read(slot, &record);
        if (ret < 0) {
            goto out;
        }
        if (ret == 0) {
            continue;
        }
        if (terminal_receipt_expired(slot, now_ms)) {
            ret = terminal_receipt_expire_slot_locked(slot);
            if (ret < 0) {
                goto out;
            }
            continue;
        }
        if (record.src_id != original_packet.src_id ||
            !semantic_digest_equal(record.semantic_digest,
                                   original_digest,
                                   sizeof(original_digest))) {
            continue;
        }
        if (matched_slot >= 0) {
            ret = -EBADMSG;
            goto out;
        }
        matched_slot = slot;
    }
    if (matched_slot >= 0) {
        ret = terminal_receipt_expire_slot_locked((uint8_t)matched_slot);
        if (ret == 0) {
            ret = 1;
        }
        goto out;
    }
    /* A valid late or stale confirm is statelessly ACKable and cannot touch
     * another retained source identity. */
    ret = 0;

out:
    k_mutex_unlock(&gateway_terminal_receipt_mutex);
    return ret;
}

#if defined(CONFIG_ZTEST)
void app_gateway_terminal_receipts_test_reset_runtime(void)
{
    k_mutex_lock(&gateway_terminal_receipt_mutex, K_FOREVER);
    gateway_terminal_receipts_initialized = false;
    memset(gateway_terminal_receipt_record_store,
           0,
           sizeof(gateway_terminal_receipt_record_store));
    memset(gateway_terminal_receipt_observed_at_ms,
           0,
           sizeof(gateway_terminal_receipt_observed_at_ms));
    k_mutex_unlock(&gateway_terminal_receipt_mutex);
}
#endif
