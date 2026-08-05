#include "app_gateway_collection_receipts.h"

#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_persistence.h"
#include "gateway_command.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_gateway_collection_receipts, LOG_LEVEL_INF);

#define APP_GATEWAY_COLLECTION_RECEIPT_MAGIC UINT32_C(0x47435231)
#define APP_GATEWAY_COLLECTION_RECEIPT_VERSION 2u
#define APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_VERSION 1u
#define APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_RECORD_SIZE 46u

struct __packed app_gateway_collection_receipt_record {
    uint32_t magic;
    uint8_t version;
    uint8_t valid;
    uint16_t size;
    uint64_t gateway_id;
    uint64_t node_id;
    uint32_t command_seq;
    uint32_t node_boot_counter;
    uint32_t collection_epoch_id;
    uint16_t gateway_epoch;
    uint16_t result_seq;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t payload_len;
    uint16_t checksum;
};

struct __packed app_gateway_collection_receipt_legacy_record {
    uint32_t magic;
    uint8_t version;
    uint8_t valid;
    uint16_t size;
    uint64_t gateway_id;
    uint64_t node_id;
    uint32_t command_seq;
    uint32_t node_boot_counter;
    uint32_t collection_epoch_id;
    uint16_t gateway_epoch;
    uint16_t result_seq;
    uint16_t payload_crc;
    uint16_t payload_len;
    uint16_t checksum;
};

BUILD_ASSERT(APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES ==
             GATEWAY_COLLECTION_RESULT_CACHE_SIZE,
             "receipt slots must cover the complete gateway collection roster");
BUILD_ASSERT(sizeof(struct app_gateway_collection_receipt) == 72u,
             "collection receipt RAM scratch must stay bounded");
BUILD_ASSERT(sizeof(struct app_gateway_collection_receipt_legacy_record) ==
             APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_RECORD_SIZE,
             "legacy collection receipt decoder layout changed");
BUILD_ASSERT(sizeof(struct app_gateway_collection_receipt_record) ==
             APP_GATEWAY_COLLECTION_RECEIPT_RECORD_SIZE,
             "collection receipt NVS schema size changed");
BUILD_ASSERT(offsetof(struct app_gateway_collection_receipt_record,
                      gateway_id) == 8u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      node_id) == 16u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      command_seq) == 24u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      collection_epoch_id) == 32u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      gateway_epoch) == 36u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      payload_digest) == 40u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      payload_len) == 72u &&
             offsetof(struct app_gateway_collection_receipt_record,
                      checksum) == 74u,
             "collection receipt NVS schema offsets changed");

K_MUTEX_DEFINE(gateway_collection_receipt_mutex);

static bool command_result_id_equal(const struct command_result_id *left,
                                    const struct command_result_id *right)
{
    return left != NULL &&
           right != NULL &&
           left->gateway_id == right->gateway_id &&
           left->gateway_epoch == right->gateway_epoch &&
           left->command_seq == right->command_seq &&
           left->node_id == right->node_id &&
           left->node_boot_counter == right->node_boot_counter &&
           left->result_seq == right->result_seq;
}

bool app_gateway_collection_receipt_valid(
    const struct app_gateway_collection_receipt *receipt)
{
    return receipt != NULL &&
           receipt->result_id.gateway_id != 0u &&
           receipt->result_id.gateway_epoch != 0u &&
           receipt->result_id.command_seq != 0u &&
           receipt->result_id.node_id != 0u &&
           receipt->result_id.node_boot_counter != 0u &&
           receipt->result_id.result_seq != 0u &&
           receipt->collection_epoch_id != 0u &&
           receipt->payload_len != 0u &&
           receipt->payload_len <= PACKET_EXT_MAX_PAYLOAD_LEN;
}

bool app_gateway_collection_receipt_same_result(
    const struct app_gateway_collection_receipt *left,
    const struct app_gateway_collection_receipt *right)
{
    return app_gateway_collection_receipt_valid(left) &&
           app_gateway_collection_receipt_valid(right) &&
           command_result_id_equal(&left->result_id, &right->result_id) &&
           left->collection_epoch_id == right->collection_epoch_id &&
           left->payload_len == right->payload_len &&
           semantic_digest_equal(left->payload_digest,
                                 right->payload_digest,
                                 sizeof(left->payload_digest));
}

bool app_gateway_collection_receipt_equal(
    const struct app_gateway_collection_receipt *left,
    const struct app_gateway_collection_receipt *right)
{
    return app_gateway_collection_receipt_same_result(left, right);
}

static uint16_t receipt_record_checksum(
    const struct app_gateway_collection_receipt_record *record)
{
    struct app_gateway_collection_receipt_record copy;

    if (record == NULL) {
        return 0u;
    }
    copy = *record;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static void receipt_record_encode(
    const struct app_gateway_collection_receipt *receipt,
    struct app_gateway_collection_receipt_record *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = APP_GATEWAY_COLLECTION_RECEIPT_MAGIC;
    record->version = APP_GATEWAY_COLLECTION_RECEIPT_VERSION;
    record->size = sizeof(*record);
    record->gateway_id = receipt->result_id.gateway_id;
    record->node_id = receipt->result_id.node_id;
    record->command_seq = receipt->result_id.command_seq;
    record->node_boot_counter = receipt->result_id.node_boot_counter;
    record->collection_epoch_id = receipt->collection_epoch_id;
    record->gateway_epoch = receipt->result_id.gateway_epoch;
    record->result_seq = receipt->result_id.result_seq;
    memcpy(record->payload_digest,
           receipt->payload_digest,
           sizeof(record->payload_digest));
    record->payload_len = receipt->payload_len;
    record->valid = 1u;
    record->checksum = receipt_record_checksum(record);
}

static int receipt_record_decode(
    const struct app_gateway_collection_receipt_record *record,
    struct app_gateway_collection_receipt *receipt)
{
    struct app_gateway_collection_receipt decoded = {0};

    if (record == NULL ||
        record->magic != APP_GATEWAY_COLLECTION_RECEIPT_MAGIC ||
        record->version != APP_GATEWAY_COLLECTION_RECEIPT_VERSION ||
        record->size != sizeof(*record) ||
        record->valid != 1u ||
        record->checksum != receipt_record_checksum(record)) {
        return -EBADMSG;
    }

    decoded.result_id.gateway_id = record->gateway_id;
    decoded.result_id.gateway_epoch = record->gateway_epoch;
    decoded.result_id.command_seq = record->command_seq;
    decoded.result_id.node_id = record->node_id;
    decoded.result_id.node_boot_counter = record->node_boot_counter;
    decoded.result_id.result_seq = record->result_seq;
    decoded.collection_epoch_id = record->collection_epoch_id;
    memcpy(decoded.payload_digest,
           record->payload_digest,
           sizeof(decoded.payload_digest));
    decoded.payload_len = record->payload_len;
    if (!app_gateway_collection_receipt_valid(&decoded)) {
        return -EBADMSG;
    }
    if (receipt != NULL) {
        *receipt = decoded;
    }
    return 0;
}

static int receipt_slot_read(
    uint8_t slot,
    struct app_gateway_collection_receipt *receipt)
{
    union {
        struct app_gateway_collection_receipt_record current;
        struct app_gateway_collection_receipt_legacy_record legacy;
    } stored;
    size_t stored_len = 0u;
    int ret;

    memset(&stored, 0, sizeof(stored));
    ret = app_mesh_persistence_read_gateway_collection_receipt(
        slot, &stored, sizeof(stored), &stored_len);
    if (ret <= 0) {
        return ret;
    }
    if (stored_len == APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_RECORD_SIZE) {
        const struct app_gateway_collection_receipt_legacy_record *legacy =
            &stored.legacy;
        struct app_gateway_collection_receipt_legacy_record copy;

        copy = *legacy;
        copy.checksum = 0u;
        if (legacy->magic != APP_GATEWAY_COLLECTION_RECEIPT_MAGIC ||
            legacy->version != APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_VERSION ||
            legacy->valid != 1u ||
            legacy->size != APP_GATEWAY_COLLECTION_RECEIPT_LEGACY_RECORD_SIZE ||
            legacy->checksum !=
                proto_crc16_ccitt_false((const uint8_t *)&copy,
                                        sizeof(copy))) {
            return -EBADMSG;
        }
        /*
         * Schema 1 retained only CRC16, so it cannot prove byte identity.
         * Retiring a valid legacy receipt permits one conservative host
         * redelivery instead of suppressing a distinct CRC collision.
         */
        ret = app_mesh_persistence_delete_gateway_collection_receipt(slot);
        return ret < 0 ? ret : 0;
    }
    if (stored_len != sizeof(stored.current)) {
        return -EBADMSG;
    }
    ret = receipt_record_decode(&stored.current, receipt);
    if (ret < 0) {
        LOG_ERR("gateway collection receipt slot %u is corrupt",
                (unsigned int)slot);
        return ret;
    }
    return 1;
}

struct receipt_scan {
    struct app_gateway_collection_receipt found;
    uint8_t found_slot;
    uint8_t free_slot;
    bool found_valid;
    bool free_valid;
};

static int receipt_store_scan(uint64_t node_id,
                              uint64_t expected_gateway_id,
                              struct receipt_scan *scan)
{
    if (node_id == 0u || scan == NULL) {
        return -EINVAL;
    }
    memset(scan, 0, sizeof(*scan));

    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES;
         slot++) {
        struct app_gateway_collection_receipt candidate = {0};
        int ret = receipt_slot_read(slot, &candidate);

        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            if (!scan->free_valid) {
                scan->free_slot = slot;
                scan->free_valid = true;
            }
            continue;
        }
        if (ret != 1) {
            return -EIO;
        }
        if (expected_gateway_id != 0u &&
            candidate.result_id.gateway_id != expected_gateway_id) {
            return -ESTALE;
        }
        if (candidate.result_id.node_id != node_id) {
            continue;
        }
        if (scan->found_valid) {
            LOG_ERR("duplicate gateway collection receipt node 0x%llx",
                    (unsigned long long)node_id);
            return -EBADMSG;
        }
        scan->found = candidate;
        scan->found_slot = slot;
        scan->found_valid = true;
    }
    return 0;
}

static bool command_sequence_strictly_newer(
    uint32_t candidate,
    uint32_t reference)
{
    uint32_t delta;

    if (candidate == 0u || reference == 0u) {
        return false;
    }
    delta = candidate - reference;
    return delta != 0u && delta < UINT32_C(0x80000000);
}

static bool receipt_is_later_collection(
    const struct app_gateway_collection_receipt *prior,
    const struct app_gateway_collection_receipt *later)
{
    return prior != NULL &&
           later != NULL &&
           prior->result_id.gateway_id == later->result_id.gateway_id &&
           prior->result_id.node_id == later->result_id.node_id &&
           !app_gateway_collection_receipt_equal(prior, later) &&
           command_sequence_strictly_newer(
               later->result_id.command_seq,
               prior->result_id.command_seq);
}

static int receipt_slot_write_and_verify(
    uint8_t slot,
    const struct app_gateway_collection_receipt *receipt)
{
    struct app_gateway_collection_receipt_record record;
    struct app_gateway_collection_receipt restored;
    int ret;

    receipt_record_encode(receipt, &record);
    ret = app_mesh_persistence_write_gateway_collection_receipt(
        slot, &record, sizeof(record));
    if (ret < 0) {
        return ret;
    }
    ret = receipt_slot_read(slot, &restored);
    if (ret != 1) {
        return ret < 0 ? ret : -EIO;
    }
    return app_gateway_collection_receipt_equal(receipt, &restored) ?
           0 : -EIO;
}

static int receipt_lookup_for_gateway(
    uint64_t node_id,
    uint64_t expected_gateway_id,
    struct app_gateway_collection_receipt *receipt)
{
    struct receipt_scan scan;
    int ret;

    if (node_id == 0u || receipt == NULL) {
        return -EINVAL;
    }
    memset(receipt, 0, sizeof(*receipt));
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    ret = k_mutex_lock(&gateway_collection_receipt_mutex, K_FOREVER);
    if (ret != 0) {
        return ret < 0 ? ret : -EIO;
    }
    ret = receipt_store_scan(node_id, expected_gateway_id, &scan);
    if (ret != 0) {
        ret = ret < 0 ? ret : -EIO;
    } else if (scan.found_valid) {
        *receipt = scan.found;
        ret = 1;
    }
    k_mutex_unlock(&gateway_collection_receipt_mutex);
    return ret;
}

int app_gateway_collection_receipts_lookup(
    uint64_t node_id,
    struct app_gateway_collection_receipt *receipt)
{
    return receipt_lookup_for_gateway(node_id, 0u, receipt);
}

int app_gateway_collection_receipts_record(
    const struct app_gateway_collection_receipt *receipt,
    const struct app_gateway_collection_receipt *superseded)
{
    struct receipt_scan scan;
    int ret;

    if (!app_gateway_collection_receipt_valid(receipt) ||
        (superseded != NULL &&
         (!app_gateway_collection_receipt_valid(superseded) ||
          superseded->result_id.node_id != receipt->result_id.node_id ||
          superseded->result_id.gateway_id !=
              receipt->result_id.gateway_id))) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    ret = k_mutex_lock(&gateway_collection_receipt_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    ret = receipt_store_scan(receipt->result_id.node_id,
                             receipt->result_id.gateway_id,
                             &scan);
    if (ret < 0) {
        goto out;
    }
    if (scan.found_valid &&
        app_gateway_collection_receipt_equal(&scan.found, receipt)) {
        ret = 0;
        goto out;
    }
    if (scan.found_valid) {
        if (superseded == NULL) {
            ret = -EALREADY;
            goto out;
        }
        if (!app_gateway_collection_receipt_equal(&scan.found, superseded)) {
            ret = -ESTALE;
            goto out;
        }
        if (!receipt_is_later_collection(superseded, receipt)) {
            ret = -EINVAL;
            goto out;
        }
        ret = receipt_slot_write_and_verify(scan.found_slot, receipt);
        goto out;
    }
    if (superseded != NULL) {
        ret = -ESTALE;
        goto out;
    }
    if (!scan.free_valid) {
        ret = -ENOSPC;
        goto out;
    }
    ret = receipt_slot_write_and_verify(scan.free_slot, receipt);

out:
    k_mutex_unlock(&gateway_collection_receipt_mutex);
    return ret;
}

struct collection_notification_info {
    struct command_result_id single_result_id;
    size_t first_record_offset;
    uint32_t collection_epoch_id;
    uint8_t record_count;
    bool collection;
    bool bundle;
};

static int collection_command_result_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool *collection,
    struct command_result_id *result_id,
    uint32_t *collection_epoch_id)
{
    enum command_id command_id;
    enum command_status status;
    const uint8_t *epoch_raw = NULL;
    uint8_t epoch_len = 0u;
    uint8_t reason = 0u;
    int ret;

    if (packet == NULL || payload == NULL || collection == NULL ||
        result_id == NULL || collection_epoch_id == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->payload_len != payload_len ||
        payload_len == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }
    *collection = false;
    memset(result_id, 0, sizeof(*result_id));
    *collection_epoch_id = 0u;

    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK ||
        app_mesh_gateway_command_flow_decode_result(command_id,
                                                     payload,
                                                     payload_len,
                                                     &status,
                                                     &reason) != PROTO_OK) {
        return -EBADMSG;
    }
    ARG_UNUSED(status);
    ARG_UNUSED(reason);

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          &epoch_raw,
                          &epoch_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK ||
        epoch_len != sizeof(uint32_t) ||
        proto_get_u32_le(epoch_raw) == 0u ||
        command_result_id_from_tlvs(payload,
                                    payload_len,
                                    result_id) != PROTO_OK ||
        result_id->gateway_id == 0u ||
        result_id->gateway_id != packet->dst_id ||
        result_id->gateway_epoch == 0u ||
        result_id->command_seq == 0u ||
        result_id->command_seq != packet->session_id ||
        result_id->node_id == 0u ||
        result_id->node_id != packet->src_id ||
        result_id->node_boot_counter == 0u ||
        result_id->result_seq == 0u) {
        return -EBADMSG;
    }

    *collection_epoch_id = proto_get_u32_le(epoch_raw);
    *collection = true;
    return 0;
}

static int collection_bundle_first_record_offset(
    const uint8_t *payload,
    size_t payload_len,
    size_t *record_offset)
{
    size_t offset = 0u;

    if (payload == NULL || record_offset == NULL) {
        return -EINVAL;
    }
    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return -EBADMSG;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        if (payload_len - offset - PROTO_TLV_HEADER_LEN < len) {
            return -EBADMSG;
        }
        if (type == TLV_RESULT_RECORD) {
            *record_offset = offset;
            return 0;
        }
        offset += PROTO_TLV_HEADER_LEN + (size_t)len;
    }
    return -EBADMSG;
}

static int collection_bundle_record_at(
    const uint8_t *payload,
    size_t payload_len,
    size_t first_record_offset,
    uint8_t record_index,
    struct result_bundle_record *record)
{
    size_t cursor = first_record_offset;

    if (payload == NULL || record == NULL) {
        return -EINVAL;
    }
    for (uint8_t i = 0u; i <= record_index; i++) {
        size_t before = cursor;
        int ret;

        if (cursor >= payload_len ||
            payload[cursor] != TLV_RESULT_RECORD) {
            return -EBADMSG;
        }
        ret = result_bundle_record_next_from_tlvs(payload,
                                                  payload_len,
                                                  &cursor,
                                                  record);
        if (ret != PROTO_OK || cursor <= before) {
            return -EBADMSG;
        }
    }
    return 0;
}

static int collection_bundle_record_validate(
    const struct result_bundle_header *bundle,
    const struct result_bundle_record *record)
{
    struct proto_packet inner_packet;
    struct command_result_id payload_id;
    uint32_t collection_epoch_id = 0u;
    bool collection = false;
    int ret;

    if (bundle == NULL || record == NULL ||
        record->payload == NULL ||
        record->result_id.gateway_id != bundle->gateway_id ||
        record->result_id.gateway_epoch != bundle->gateway_epoch ||
        record->result_id.command_seq != bundle->command_seq ||
        record->result_id.node_id == 0u ||
        record->result_id.node_boot_counter == 0u ||
        record->result_id.result_seq == 0u) {
        return -EBADMSG;
    }

    memset(&inner_packet, 0, sizeof(inner_packet));
    inner_packet.msg_type = MSG_COMMAND_RESULT;
    inner_packet.src_id = record->result_id.node_id;
    inner_packet.dst_id = bundle->gateway_id;
    inner_packet.session_id = bundle->command_seq;
    inner_packet.payload_len = record->payload_len;
    ret = collection_command_result_validate(&inner_packet,
                                             record->payload,
                                             record->payload_len,
                                             &collection,
                                             &payload_id,
                                             &collection_epoch_id);
    if (ret < 0 ||
        !collection ||
        !command_result_id_equal(&record->result_id, &payload_id) ||
        collection_epoch_id != bundle->collection_epoch_id) {
        return -EBADMSG;
    }
    return 0;
}

static int collection_bundle_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct collection_notification_info *info)
{
    struct result_bundle_header bundle;
    size_t cursor;
    uint8_t parsed_count = 0u;
    int ret;

    if (packet == NULL || payload == NULL || info == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE ||
        packet->payload_len != payload_len ||
        payload_len == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }
    ret = result_bundle_header_from_tlvs(payload, payload_len, &bundle);
    if (ret != PROTO_OK ||
        bundle.gateway_id == 0u ||
        bundle.gateway_id != packet->dst_id ||
        bundle.gateway_epoch == 0u ||
        bundle.command_seq == 0u ||
        bundle.command_seq != packet->session_id ||
        bundle.collection_epoch_id == 0u ||
        bundle.record_count == 0u ||
        bundle.record_count > COLLECTION_BUNDLE_MAX_RECORDS) {
        return -EBADMSG;
    }
    ret = collection_bundle_first_record_offset(payload,
                                                payload_len,
                                                &cursor);
    if (ret < 0 ||
        proto_crc16_ccitt_false(&payload[cursor],
                                payload_len - cursor) !=
            bundle.bundle_crc) {
        return -EBADMSG;
    }
    info->first_record_offset = cursor;

    while (cursor < payload_len) {
        struct result_bundle_record record;
        struct command_result_id current_id;
        uint16_t current_payload_len;
        size_t before = cursor;

        if (payload[cursor] != TLV_RESULT_RECORD ||
            parsed_count >= bundle.record_count ||
            result_bundle_record_next_from_tlvs(payload,
                                                payload_len,
                                                &cursor,
                                                &record) != PROTO_OK ||
            cursor <= before ||
            collection_bundle_record_validate(&bundle, &record) < 0) {
            return -EBADMSG;
        }

        current_id = record.result_id;
        current_payload_len = record.payload_len;
        for (uint8_t prior_index = 0u;
             prior_index < parsed_count;
             prior_index++) {
            struct result_bundle_record prior;

            ret = collection_bundle_record_at(
                payload,
                payload_len,
                info->first_record_offset,
                prior_index,
                &prior);
            if (ret < 0) {
                return ret;
            }
            if (prior.result_id.node_id == current_id.node_id &&
                (!command_result_id_equal(&prior.result_id, &current_id) ||
                 prior.payload_len != current_payload_len ||
                 memcmp(prior.payload,
                        record.payload,
                        current_payload_len) != 0)) {
                return -EBADMSG;
            }
        }
        parsed_count++;
    }
    if (parsed_count != bundle.record_count) {
        return -EBADMSG;
    }

    info->collection_epoch_id = bundle.collection_epoch_id;
    info->record_count = bundle.record_count;
    info->collection = true;
    info->bundle = true;
    return 0;
}

static int collection_notification_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct collection_notification_info *info)
{
    int ret;

    if (packet == NULL || info == NULL) {
        return -EINVAL;
    }
    memset(info, 0, sizeof(*info));
    if (packet->msg_type != MSG_COMMAND_RESULT &&
        packet->msg_type != MSG_RESULT_BUNDLE) {
        return 0;
    }
    if (payload == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        return collection_bundle_validate(packet,
                                          payload,
                                          payload_len,
                                          info);
    }

    ret = collection_command_result_validate(
        packet,
        payload,
        payload_len,
        &info->collection,
        &info->single_result_id,
        &info->collection_epoch_id);
    if (ret < 0) {
        return ret;
    }
    info->record_count = info->collection ? 1u : 0u;
    return 0;
}

static void receipt_from_result(
    const struct command_result_id *result_id,
    uint32_t collection_epoch_id,
    const uint8_t *payload,
    size_t payload_len,
    struct app_gateway_collection_receipt *receipt)
{
    memset(receipt, 0, sizeof(*receipt));
    receipt->result_id = *result_id;
    receipt->collection_epoch_id = collection_epoch_id;
    if (payload_len <= UINT16_MAX &&
        semantic_digest_sha256(payload,
                               payload_len,
                               receipt->payload_digest)) {
        receipt->payload_len = (uint16_t)payload_len;
    }
}

static int receipt_record_host_result(
    const struct command_result_id *result_id,
    uint32_t collection_epoch_id,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_gateway_collection_receipt receipt;
    struct app_gateway_collection_receipt superseded;
    int ret;

    receipt_from_result(result_id,
                        collection_epoch_id,
                        payload,
                        payload_len,
                        &receipt);
    if (!app_gateway_collection_receipt_valid(&receipt)) {
        return -EBADMSG;
    }
    ret = receipt_lookup_for_gateway(result_id->node_id,
                                     result_id->gateway_id,
                                     &superseded);
    if (ret < 0) {
        return ret;
    }
    if (ret == 0) {
        return app_gateway_collection_receipts_record(&receipt, NULL);
    }
    if (app_gateway_collection_receipt_equal(&receipt, &superseded)) {
        return 0;
    }
    return app_gateway_collection_receipts_record(&receipt, &superseded);
}

int app_gateway_collection_receipts_record_host_notification(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t host_projection_mask)
{
    struct collection_notification_info info;
    int ret;

    ret = collection_notification_validate(packet,
                                           payload,
                                           payload_len,
                                           &info);
    if (ret < 0 || !info.collection) {
        return ret;
    }
    if (!info.bundle) {
        if (host_projection_mask != 0u) {
            return -EBADMSG;
        }
        return receipt_record_host_result(
            &info.single_result_id,
            info.collection_epoch_id,
            payload,
            payload_len);
    }

    {
        uint8_t valid_mask =
            info.record_count == 8u ?
            UINT8_MAX :
            (uint8_t)((1u << info.record_count) - 1u);
        size_t cursor = info.first_record_offset;

        if (host_projection_mask != 0u &&
            (host_projection_mask & (uint8_t)~valid_mask) != 0u) {
            return -EBADMSG;
        }
        for (uint8_t record_index = 0u;
             record_index < info.record_count;
             record_index++) {
            struct result_bundle_record record;
            size_t before = cursor;
            bool selected =
                host_projection_mask == 0u ||
                (host_projection_mask &
                 (uint8_t)(1u << record_index)) != 0u;

            if (result_bundle_record_next_from_tlvs(payload,
                                                    payload_len,
                                                    &cursor,
                                                    &record) != PROTO_OK ||
                cursor <= before) {
                return -EBADMSG;
            }
            if (!selected) {
                continue;
            }
            ret = receipt_record_host_result(
                &record.result_id,
                info.collection_epoch_id,
                record.payload,
                record.payload_len);
            if (ret < 0) {
                return ret;
            }
        }
        return cursor == payload_len ? 0 : -EBADMSG;
    }
}

static int receipt_classify_result(
    const struct app_gateway_collection_receipt *incoming)
{
    struct app_gateway_collection_receipt stored = {0};
    int ret;

    ret = receipt_lookup_for_gateway(
        incoming->result_id.node_id,
        incoming->result_id.gateway_id,
        &stored);
    if (ret <= 0) {
        return ret;
    }
    if (stored.result_id.gateway_id != incoming->result_id.gateway_id ||
        stored.result_id.node_id != incoming->result_id.node_id) {
        return -ESTALE;
    }
    if (stored.result_id.command_seq ==
        incoming->result_id.command_seq) {
        return app_gateway_collection_receipt_equal(&stored, incoming) ?
               1 : -EBADMSG;
    }
    if (command_sequence_strictly_newer(
            stored.result_id.command_seq,
            incoming->result_id.command_seq)) {
        return 1;
    }
    if (command_sequence_strictly_newer(
            incoming->result_id.command_seq,
            stored.result_id.command_seq)) {
        return 0;
    }
    return -ESTALE;
}

int app_gateway_collection_receipts_classify_retry(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct collection_notification_info info;
    bool proof_seen = false;
    bool missing_seen = false;
    int ret;

    ret = collection_notification_validate(packet,
                                           payload,
                                           payload_len,
                                           &info);
    if (ret < 0 || !info.collection) {
        return ret;
    }
    if (!info.bundle) {
        struct app_gateway_collection_receipt incoming;

        receipt_from_result(
            &info.single_result_id,
            info.collection_epoch_id,
            payload,
            payload_len,
            &incoming);
        return receipt_classify_result(&incoming);
    }

    {
        size_t cursor = info.first_record_offset;

        for (uint8_t record_index = 0u;
             record_index < info.record_count;
             record_index++) {
            struct app_gateway_collection_receipt incoming;
            struct result_bundle_record record;
            size_t before = cursor;

            if (result_bundle_record_next_from_tlvs(payload,
                                                    payload_len,
                                                    &cursor,
                                                    &record) != PROTO_OK ||
                cursor <= before) {
                return -EBADMSG;
            }
            receipt_from_result(&record.result_id,
                                info.collection_epoch_id,
                                record.payload,
                                record.payload_len,
                                &incoming);
            ret = receipt_classify_result(&incoming);
            if (ret < 0) {
                return ret;
            }
            proof_seen = proof_seen || ret == 1;
            missing_seen = missing_seen || ret == 0;
        }
        if (cursor != payload_len) {
            return -EBADMSG;
        }
    }
    return proof_seen && missing_seen ? -ESTALE :
           proof_seen ? 1 : 0;
}
