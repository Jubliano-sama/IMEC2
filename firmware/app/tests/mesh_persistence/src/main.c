#include "app_discovery_assignment_policy.h"
#include "app_gateway_collection_receipts.h"
#include "app_gateway_terminal_receipts.h"
#include "app_mesh_persistence.h"
#include "app_mesh_preemption.h"
#include "app_mesh_result_handoff.h"
#include "mesh.h"
#include "protocol.h"
#include "route.h"

#include <string.h>
#include <errno.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define LOCAL_ID 0x1111222233334444ull
#define GATEWAY_ID 0x9999888877776666ull
#define CHILD_ID 0x5555666677778888ull
#define MEMBER_A_ID 0x0102030405060708ull
#define MEMBER_B_ID 0x1112131415161718ull
#define MEMBER_C_ID 0x2122232425262728ull
#define MEMBER_D_ID 0x3132333435363738ull

static struct k_work_delayable test_tx_timeout_work;

static bool test_command_result_id_equal(
    const struct command_result_id *left,
    const struct command_result_id *right)
{
    return left->gateway_id == right->gateway_id &&
           left->gateway_epoch == right->gateway_epoch &&
           left->command_seq == right->command_seq &&
           left->node_id == right->node_id &&
           left->node_boot_counter == right->node_boot_counter &&
           left->result_seq == right->result_seq;
}

static void test_payload_digest(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    zassert_true(semantic_digest_sha256(payload, payload_len, digest));
}

static struct app_gateway_collection_receipt make_collection_receipt(
    uint64_t node_id,
    uint32_t command_seq,
    uint32_t collection_epoch_id,
    uint16_t result_seq)
{
    struct app_gateway_collection_receipt receipt = {
        .result_id = {
            .gateway_id = GATEWAY_ID,
            .gateway_epoch = 13u,
            .command_seq = command_seq,
            .node_id = node_id,
            .node_boot_counter = 31u,
            .result_seq = result_seq,
        },
        .collection_epoch_id = collection_epoch_id,
        .payload_len = 72u,
    };

    zassert_true(semantic_digest_sha256(&result_seq,
                                        sizeof(result_seq),
                                        receipt.payload_digest));
    return receipt;
}

static size_t build_collection_command_result(
    uint8_t *payload,
    size_t payload_cap,
    const struct command_result_id *result_id,
    uint32_t collection_epoch_id,
    uint8_t reason)
{
    size_t payload_len = 0u;

    zassert_equal(
        gateway_command_append_collection_result_identity(
            payload,
            payload_cap,
            &payload_len,
            result_id,
            collection_epoch_id),
        PROTO_OK);
    zassert_equal(tlv_append_u16(payload,
                                 payload_cap,
                                 &payload_len,
                                 TLV_COMMAND_ID,
                                 CMD_GET_STATUS),
                  PROTO_OK);
    zassert_equal(tlv_append_u16(payload,
                                 payload_cap,
                                 &payload_len,
                                 TLV_COMMAND_STATUS,
                                 COMMAND_OK),
                  PROTO_OK);
    zassert_equal(tlv_append_u8(payload,
                                payload_cap,
                                &payload_len,
                                TLV_REASON,
                                reason),
                  PROTO_OK);
    return payload_len;
}

static size_t build_ordinary_command_result(uint8_t *payload,
                                            size_t payload_cap)
{
    size_t payload_len = 0u;

    zassert_equal(tlv_append_u16(payload,
                                 payload_cap,
                                 &payload_len,
                                 TLV_COMMAND_ID,
                                 CMD_PING),
                  PROTO_OK);
    zassert_equal(tlv_append_u16(payload,
                                 payload_cap,
                                 &payload_len,
                                 TLV_COMMAND_STATUS,
                                 COMMAND_OK),
                  PROTO_OK);
    zassert_equal(tlv_append_u8(payload,
                                payload_cap,
                                &payload_len,
                                TLV_REASON,
                                0u),
                  PROTO_OK);
    return payload_len;
}

struct collection_bundle_test_record {
    struct command_result_id result_id;
    const uint8_t *payload;
    uint16_t payload_len;
};

static size_t build_collection_result_bundle(
    uint8_t *payload,
    size_t payload_cap,
    uint64_t gateway_id,
    uint16_t gateway_epoch,
    uint32_t command_seq,
    uint32_t collection_epoch_id,
    const struct collection_bundle_test_record *records,
    uint8_t record_count)
{
    struct result_bundle_header header = {
        .gateway_id = gateway_id,
        .gateway_epoch = gateway_epoch,
        .command_seq = command_seq,
        .collection_epoch_id = collection_epoch_id,
        .bundle_id = 17u,
        .record_count = record_count,
    };
    size_t payload_len = 0u;
    size_t records_offset;
    size_t rewritten_len = 0u;

    zassert_not_null(payload);
    zassert_not_null(records);
    zassert_true(record_count > 0u);
    zassert_equal(result_bundle_header_append_tlvs(payload,
                                                   payload_cap,
                                                   &payload_len,
                                                   &header),
                  PROTO_OK);
    records_offset = payload_len;
    for (uint8_t i = 0u; i < record_count; i++) {
        const struct result_bundle_record record = {
            .result_id = records[i].result_id,
            .payload_len = records[i].payload_len,
            .payload_crc = proto_crc16_ccitt_false(records[i].payload,
                                                   records[i].payload_len),
            .payload = records[i].payload,
        };

        zassert_equal(result_bundle_record_append_tlv(payload,
                                                      payload_cap,
                                                      &payload_len,
                                                      &record),
                      PROTO_OK);
    }
    header.bundle_crc = proto_crc16_ccitt_false(
        &payload[records_offset],
        payload_len - records_offset);
    zassert_equal(result_bundle_header_append_tlvs(payload,
                                                   payload_cap,
                                                   &rewritten_len,
                                                   &header),
                  PROTO_OK);
    zassert_equal(rewritten_len, records_offset);
    return payload_len;
}

static struct proto_packet make_collection_result_packet(
    const struct command_result_id *result_id,
    uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = result_id->node_id,
        .dst_id = result_id->gateway_id,
        .session_id = result_id->command_seq,
        .seq = result_id->result_seq,
        .ttl = 5u,
        .payload_len = payload_len,
    };
}

static struct proto_packet make_collection_bundle_packet(
    uint64_t gateway_id,
    uint32_t command_seq,
    uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_RESULT_BUNDLE,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = CHILD_ID,
        .dst_id = gateway_id,
        .session_id = command_seq,
        .seq = 91u,
        .ttl = 5u,
        .payload_len = payload_len,
    };
}

static void clear_collection_receipts(void)
{
    app_mesh_persistence_test_reset_faults();
    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_gateway_collection_receipt(slot));
    }
}

static void clear_terminal_receipts(void)
{
    app_mesh_persistence_test_reset_faults();
    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_gateway_terminal_receipt(slot));
    }
    app_gateway_terminal_receipts_test_reset_runtime();
}

static struct proto_packet make_gateway_host_packet(uint8_t msg_type,
                                                    uint16_t seq,
                                                    uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = msg_type,
        .flags = FLAG_GATEWAY_ACK_REQUIRED |
                 (msg_type == MSG_CLICK_REPORT ? FLAG_COUNT_AS_CLICK : 0u),
        .src_id = CHILD_ID,
        .dst_id = GATEWAY_ID,
        .session_id = 0x12345678u,
        .seq = seq,
        .ttl = 5u,
        .payload_len = payload_len,
        .message_age_ms = 17u,
    };
}

static struct proto_packet make_gateway_click_packet(uint16_t seq,
                                                     uint16_t payload_len)
{
    return make_gateway_host_packet(MSG_CLICK_REPORT, seq, payload_len);
}

static void make_gateway_ack_confirm(
    const struct proto_packet *original,
    const uint8_t *original_payload,
    size_t original_payload_len,
    struct proto_packet *confirm,
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN],
    size_t *confirm_payload_len)
{
    zassert_equal(mesh_gateway_ack_confirm_payload_build(
                      original,
                      original_payload,
                      original_payload_len,
                      confirm_payload,
                      MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN,
                      confirm_payload_len),
                  PROTO_OK);
    zassert_equal(mesh_init_gateway_ack_confirm(confirm,
                                                original->src_id,
                                                original->dst_id,
                                                original->session_id,
                                                original->seq),
                  PROTO_OK);
}

int app_mesh_persistence_test_write_gateway_membership_snapshot(
    const void *snapshot,
    size_t snapshot_len);
static struct gateway_membership_roster make_gateway_membership_roster(void);

static void *mesh_persistence_suite_setup(void)
{
    const struct flash_area *area = NULL;
    int ret;

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    zassert_ok(ret);
    zassert_not_null(area);
    if (ret < 0 || area == NULL) {
        return NULL;
    }

    ret = flash_area_erase(area, 0u, area->fa_size);
    flash_area_close(area);
    zassert_ok(ret);
    app_mesh_persistence_test_reset_faults();
    return NULL;
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_round_trip_and_proven_supersession)
{
    const struct app_gateway_collection_receipt first =
        make_collection_receipt(MEMBER_A_ID, 1001u, 2001u, 1u);
    const struct app_gateway_collection_receipt later =
        make_collection_receipt(MEMBER_A_ID, 1002u, 2002u, 2u);
    const struct app_gateway_collection_receipt unrelated_prior =
        make_collection_receipt(MEMBER_A_ID, 999u, 1999u, 9u);
    const struct app_gateway_collection_receipt same_sequence_conflict =
        make_collection_receipt(MEMBER_A_ID, 1002u, 2999u, 3u);
    struct app_gateway_collection_receipt restored;

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();

    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_A_ID, &restored),
        0);
    zassert_false(app_gateway_collection_receipt_valid(&restored));

    zassert_ok(app_gateway_collection_receipts_record(&first, NULL));
    memset(&restored, 0, sizeof(restored));
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_A_ID, &restored),
        1);
    zassert_true(app_gateway_collection_receipt_equal(&first, &restored));

    /* Exact replay after a process reset is write-free and idempotent. */
    zassert_ok(app_gateway_collection_receipts_record(&first, NULL));
    zassert_equal(
        app_gateway_collection_receipts_record(&later, NULL),
        -EALREADY);
    zassert_equal(
        app_gateway_collection_receipts_record(&later, &unrelated_prior),
        -ESTALE);

    zassert_ok(app_gateway_collection_receipts_record(&later, &first));
    memset(&restored, 0, sizeof(restored));
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_A_ID, &restored),
        1);
    zassert_true(app_gateway_collection_receipt_equal(&later, &restored));
    zassert_equal(
        app_gateway_collection_receipts_record(
            &same_sequence_conflict, &later),
        -EINVAL);
    zassert_equal(
        app_gateway_collection_receipts_record(&first, &later),
        -EINVAL);

    /*
     * If the replacement write completed but its caller reset before seeing
     * success, retrying with the old proof observes the exact new owner.
     */
    zassert_ok(app_gateway_collection_receipts_record(&later, &first));
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_host_notification_single_is_exact)
{
    static uint8_t payload[128];
    static uint8_t ordinary_payload[32];
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 1501u,
        .node_id = MEMBER_A_ID,
        .node_boot_counter = 51u,
        .result_seq = 9u,
    };
    struct app_gateway_collection_receipt restored;
    struct proto_packet packet;
    struct proto_packet ordinary_packet;
    size_t payload_len;
    size_t ordinary_len;

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();
    payload_len = build_collection_command_result(payload,
                                                  sizeof(payload),
                                                  &result_id,
                                                  2501u,
                                                  7u);
    packet = make_collection_result_packet(&result_id,
                                           (uint16_t)payload_len);

    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, payload, payload_len),
                  0);
    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, payload, payload_len, 0u));
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_A_ID, &restored),
                  1);
    zassert_true(test_command_result_id_equal(
        &restored.result_id, &result_id));
    zassert_equal(restored.collection_epoch_id, 2501u);
    zassert_equal(restored.payload_len, payload_len);
    {
        uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];

        zassert_true(semantic_digest_sha256(payload,
                                            payload_len,
                                            expected_digest));
        zassert_mem_equal(restored.payload_digest,
                          expected_digest,
                          sizeof(expected_digest));
    }
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, payload, payload_len),
                  1);
    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, payload, payload_len, 0u));
    zassert_equal(
        app_gateway_collection_receipts_record_host_notification(
            &packet, payload, payload_len, 1u),
        -EBADMSG);

    ordinary_len = build_ordinary_command_result(
        ordinary_payload, sizeof(ordinary_payload));
    ordinary_packet = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 17u, (uint16_t)ordinary_len);
    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &ordinary_packet,
        ordinary_payload,
        ordinary_len,
        0u));
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &ordinary_packet,
                      ordinary_payload,
                      ordinary_len),
                  0);
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_rejects_crc16_collision)
{
    static uint8_t first[128];
    static uint8_t second[128];
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 1502u,
        .node_id = MEMBER_A_ID,
        .node_boot_counter = 52u,
        .result_seq = 10u,
    };
    struct proto_packet packet;
    size_t first_len;
    size_t second_len;

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();
    first_len = build_collection_command_result(first,
                                                sizeof(first),
                                                &result_id,
                                                2502u,
                                                0u);
    zassert_equal(tlv_append_u16(first,
                                 sizeof(first),
                                 &first_len,
                                 TLV_FW_VERSION,
                                 UINT16_C(0x3037)),
                  PROTO_OK);
    second_len = build_collection_command_result(second,
                                                 sizeof(second),
                                                 &result_id,
                                                 2502u,
                                                 1u);
    zassert_equal(tlv_append_u16(second,
                                 sizeof(second),
                                 &second_len,
                                 TLV_FW_VERSION,
                                 0u),
                  PROTO_OK);
    zassert_equal(first_len, second_len);
    zassert_not_equal(memcmp(first, second, first_len), 0);
    zassert_equal(proto_crc16_ccitt_false(first, first_len),
                  proto_crc16_ccitt_false(second, second_len));
    packet = make_collection_result_packet(&result_id,
                                           (uint16_t)first_len);

    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, first, first_len, 0u));
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, first, first_len),
                  1);
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, second, second_len),
                  -EBADMSG);
    zassert_true(app_gateway_collection_receipts_record_host_notification(
                     &packet, second, second_len, 0u) < 0);
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_bundle_projection_and_partial_retry)
{
    static uint8_t result_payload_a[128];
    static uint8_t result_payload_b[128];
    static uint8_t bundle_payload[640];
    static uint8_t corrupt_bundle[640];
    const uint32_t command_seq = 1601u;
    const uint32_t collection_epoch_id = 2601u;
    struct collection_bundle_test_record records[2];
    struct app_gateway_collection_receipt restored;
    struct proto_packet packet;
    size_t result_len_a;
    size_t result_len_b;
    size_t bundle_len;

    records[0].result_id = (struct command_result_id) {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = command_seq,
        .node_id = MEMBER_A_ID,
        .node_boot_counter = 61u,
        .result_seq = 10u,
    };
    records[1].result_id = (struct command_result_id) {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = command_seq,
        .node_id = MEMBER_B_ID,
        .node_boot_counter = 62u,
        .result_seq = 11u,
    };
    result_len_a = build_collection_command_result(
        result_payload_a,
        sizeof(result_payload_a),
        &records[0].result_id,
        collection_epoch_id,
        10u);
    result_len_b = build_collection_command_result(
        result_payload_b,
        sizeof(result_payload_b),
        &records[1].result_id,
        collection_epoch_id,
        11u);
    records[0].payload = result_payload_a;
    records[0].payload_len = (uint16_t)result_len_a;
    records[1].payload = result_payload_b;
    records[1].payload_len = (uint16_t)result_len_b;
    bundle_len = build_collection_result_bundle(
        bundle_payload,
        sizeof(bundle_payload),
        GATEWAY_ID,
        13u,
        command_seq,
        collection_epoch_id,
        records,
        (uint8_t)ARRAY_SIZE(records));
    packet = make_collection_bundle_packet(
        GATEWAY_ID, command_seq, (uint16_t)bundle_len);

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();
    zassert_equal(
        app_gateway_collection_receipts_record_host_notification(
            &packet, bundle_payload, bundle_len, 0x04u),
        -EBADMSG);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_A_ID, &restored),
                  0);

    memcpy(corrupt_bundle, bundle_payload, bundle_len);
    corrupt_bundle[bundle_len - 1u] ^= 0x01u;
    zassert_equal(
        app_gateway_collection_receipts_record_host_notification(
            &packet, corrupt_bundle, bundle_len, 0u),
        -EBADMSG);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_A_ID, &restored),
                  0);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_B_ID, &restored),
                  0);

    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, bundle_payload, bundle_len, 0x02u));
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_A_ID, &restored),
                  0);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_B_ID, &restored),
                  1);
    zassert_true(test_command_result_id_equal(
        &restored.result_id, &records[1].result_id));
    zassert_true(app_gateway_collection_receipts_classify_retry(
                     &packet, bundle_payload, bundle_len) < 0);

    clear_collection_receipts();
    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, bundle_payload, bundle_len, 0x01u));
    app_mesh_persistence_test_fail_gateway_collection_receipt_write(
        -EIO, 1u);
    zassert_equal(
        app_gateway_collection_receipts_record_host_notification(
            &packet, bundle_payload, bundle_len, 0u),
        -EIO);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_A_ID, &restored),
                  1);
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_B_ID, &restored),
                  0);

    zassert_ok(app_gateway_collection_receipts_record_host_notification(
        &packet, bundle_payload, bundle_len, 0u));
    zassert_equal(app_gateway_collection_receipts_lookup(
                      MEMBER_B_ID, &restored),
                  1);
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, bundle_payload, bundle_len),
                  1);
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_retry_serial_proof_fails_closed)
{
    static uint8_t payload[128];
    const struct command_result_id incoming_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = UINT32_MAX - 1u,
        .node_id = MEMBER_C_ID,
        .node_boot_counter = 71u,
        .result_seq = 12u,
    };
    struct app_gateway_collection_receipt stored;
    struct proto_packet packet;
    size_t payload_len;

    payload_len = build_collection_command_result(
        payload,
        sizeof(payload),
        &incoming_id,
        2701u,
        12u);
    packet = make_collection_result_packet(&incoming_id,
                                           (uint16_t)payload_len);

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, payload, payload_len),
                  0);

    /* Command sequence 1 is strictly newer than UINT32_MAX - 1. */
    stored = make_collection_receipt(MEMBER_C_ID, 1u, 2702u, 13u);
    zassert_ok(app_gateway_collection_receipts_record(&stored, NULL));
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, payload, payload_len),
                  1);

    clear_collection_receipts();
    stored = make_collection_receipt(
        MEMBER_C_ID,
        incoming_id.command_seq - 1u,
        2702u,
        13u);
    zassert_ok(app_gateway_collection_receipts_record(&stored, NULL));
    zassert_equal(app_gateway_collection_receipts_classify_retry(
                      &packet, payload, payload_len),
                  0);

    clear_collection_receipts();
    stored = make_collection_receipt(
        MEMBER_C_ID,
        incoming_id.command_seq + UINT32_C(0x80000000),
        2703u,
        14u);
    zassert_ok(app_gateway_collection_receipts_record(&stored, NULL));
    zassert_true(app_gateway_collection_receipts_classify_retry(
                     &packet, payload, payload_len) < 0);

    clear_collection_receipts();
    stored = make_collection_receipt(
        MEMBER_C_ID,
        incoming_id.command_seq,
        9999u,
        incoming_id.result_seq);
    zassert_ok(app_gateway_collection_receipts_record(&stored, NULL));
    zassert_true(app_gateway_collection_receipts_classify_retry(
                     &packet, payload, payload_len) < 0);
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_corruption_and_io_fail_closed)
{
    const struct app_gateway_collection_receipt receipt =
        make_collection_receipt(MEMBER_D_ID, 1201u, 2201u, 11u);
    const uint8_t corrupt[] = {0x47u, 0x43u, 0x52u};
    struct app_gateway_collection_receipt restored;

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();
    zassert_ok(
        app_mesh_persistence_test_write_gateway_collection_receipt_raw(
            17u, corrupt, sizeof(corrupt)));

    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_D_ID, &restored),
        -EBADMSG);
    zassert_equal(
        app_gateway_collection_receipts_record(&receipt, NULL),
        -EBADMSG);
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_D_ID, &restored),
        -EBADMSG);

    clear_collection_receipts();
    app_mesh_persistence_test_fail_gateway_collection_receipt_read(
        -EIO, 1u);
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_D_ID, &restored),
        -EIO);
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_D_ID, &restored),
        0);

    app_mesh_persistence_test_fail_gateway_collection_receipt_write(
        -EIO, 1u);
    zassert_equal(
        app_gateway_collection_receipts_record(&receipt, NULL),
        -EIO);
    zassert_equal(
        app_gateway_collection_receipts_lookup(MEMBER_D_ID, &restored),
        0);
    zassert_ok(app_gateway_collection_receipts_record(&receipt, NULL));
    clear_collection_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_collection_receipt_capacity_covers_all_fifty_nodes)
{
    struct app_gateway_collection_receipt receipt;
    struct app_gateway_collection_receipt restored;

    zassert_ok(app_mesh_persistence_init());
    clear_collection_receipts();

    for (uint16_t i = 0u;
         i < APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES;
         i++) {
        receipt = make_collection_receipt(
            UINT64_C(0x6000000000000000) + i + 1u,
            1300u + i,
            2300u + i,
            (uint16_t)(i + 1u));
        zassert_ok(app_gateway_collection_receipts_record(&receipt, NULL),
                   "receipt %u did not fit",
                   i);
    }

    receipt = make_collection_receipt(
        UINT64_C(0x7000000000000001),
        1400u,
        2400u,
        51u);
    zassert_equal(
        app_gateway_collection_receipts_record(&receipt, NULL),
        -ENOSPC);

    zassert_equal(
        app_gateway_collection_receipts_lookup(
            UINT64_C(0x6000000000000001), &restored),
        1);
    zassert_equal(restored.result_id.command_seq, 1300u);
    zassert_equal(
        app_gateway_collection_receipts_lookup(
            UINT64_C(0x6000000000000032), &restored),
        1);
    zassert_equal(restored.result_id.command_seq, 1349u);
    clear_collection_receipts();
}

struct legacy_discovery_assignment_snapshot_v2 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
};

struct legacy_discovery_assignment_snapshot_v3 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
};

struct legacy_discovery_assignment_snapshot_v4 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
};

struct legacy_discovery_assignment_snapshot_v6 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint16_t table_packet_seq;
    uint16_t response_spread_ms;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint8_t ack_pending;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint32_t magic;
    uint16_t size;
    uint16_t checksum;
};

struct legacy_gateway_membership_snapshot_v2 {
    uint8_t version;
    uint8_t valid;
    uint16_t membership_epoch;
    uint16_t node_count;
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint32_t assignment_epoch;
    uint32_t assignment_table_seq;
    uint32_t assignment_table_fingerprint;
    uint32_t magic;
    uint16_t checksum;
    uint8_t assignment_proof_valid;
    uint8_t reserved;
};

struct legacy_gateway_membership_snapshot_v3 {
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    struct gateway_membership_publication publication;
    uint32_t assignment_epoch;
    uint32_t assignment_table_seq;
    uint32_t assignment_table_fingerprint;
    uint32_t magic;
    uint16_t membership_epoch;
    uint16_t checksum;
    uint8_t version;
    uint8_t node_count;
    uint8_t slot_span;
    uint8_t valid;
    uint8_t assignment_proof_valid;
    uint8_t reserved[3];
};

static uint16_t legacy_gateway_membership_v2_checksum(
    const struct legacy_gateway_membership_snapshot_v2 *snapshot)
{
    struct legacy_gateway_membership_snapshot_v2 copy = *snapshot;

    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static uint16_t legacy_gateway_membership_v3_checksum(
    const struct legacy_gateway_membership_snapshot_v3 *snapshot)
{
    struct legacy_gateway_membership_snapshot_v3 copy = *snapshot;

    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static uint16_t legacy_discovery_assignment_v6_checksum(
    const struct legacy_discovery_assignment_snapshot_v6 *snapshot)
{
    uint8_t encoded[110u];
    size_t offset = 0u;

    proto_put_u32_le(&encoded[offset], snapshot->magic);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->size);
    offset += sizeof(uint16_t);
    encoded[offset++] = snapshot->version;
    proto_put_u32_le(&encoded[offset], snapshot->epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_command_seq);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_fingerprint);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->table_packet_seq);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&encoded[offset], snapshot->response_spread_ms);
    offset += sizeof(uint16_t);
    proto_put_u64_le(&encoded[offset], snapshot->local_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&encoded[offset], snapshot->gateway_id);
    offset += sizeof(uint64_t);
    encoded[offset++] = snapshot->slot;
    encoded[offset++] = snapshot->slot_count;
    encoded[offset++] = snapshot->provisioned;
    encoded[offset++] = snapshot->valid;
    encoded[offset++] = snapshot->retired_epoch_count;
    encoded[offset++] = snapshot->ordered_epoch_valid;
    encoded[offset++] = snapshot->ack_pending;
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        proto_put_u32_le(&encoded[offset], snapshot->retired_epochs[i]);
        offset += sizeof(uint32_t);
    }
    zassert_equal(offset, sizeof(encoded));
    return proto_crc16_ccitt_false(encoded, sizeof(encoded));
}

static struct discovery_assignment_table_commitment test_table_commitment(
    uint32_t seed)
{
    struct discovery_assignment_table_commitment commitment;

    for (size_t i = 0u; i < sizeof(commitment.bytes); i++) {
        commitment.bytes[i] =
            (uint8_t)(seed >> ((i % sizeof(seed)) * 8u)) ^
            (uint8_t)(i * 29u);
    }
    return commitment;
}

ZTEST(mesh_persistence,
      test_discovery_assignment_v2_v3_v4_retire_legacy_proof)
{
    struct legacy_discovery_assignment_snapshot_v2 v2 = {
        .epoch = 77u,
        .table_command_seq = 88u,
        .table_fingerprint = 99u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = 2u,
        .slot = 1u,
        .slot_count = 3u,
        .provisioned = true,
        .valid = true,
    };
    struct legacy_discovery_assignment_snapshot_v3 v3 = {
        .epoch = 177u,
        .table_command_seq = 188u,
        .table_fingerprint = 199u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = 3u,
        .slot = 2u,
        .slot_count = 4u,
        .provisioned = true,
        .valid = true,
        .retired_epoch_count = 1u,
        .retired_epochs = {176u},
    };
    struct legacy_discovery_assignment_snapshot_v4 v4 = {
        .epoch = 277u,
        .table_command_seq = 288u,
        .table_fingerprint = 299u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = 4u,
        .slot = 3u,
        .slot_count = 5u,
        .provisioned = 1u,
        .valid = 1u,
        .retired_epoch_count = 1u,
        .ordered_epoch_valid = 1u,
        .retired_epochs = {276u},
    };
    struct app_mesh_discovery_assignment_snapshot restored;

    zassert_equal(sizeof(v2), 40u);
    zassert_equal(sizeof(v3), 104u);
    zassert_equal(sizeof(v4), 104u);
    zassert_equal(sizeof(restored), 184u);
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(
        app_mesh_persistence_test_write_assignment_snapshot(
            &v2, sizeof(v2)));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_mem_equal(&restored,
                      &(struct app_mesh_discovery_assignment_snapshot){0},
                      sizeof(restored));
    memset(&restored, 0xa5, sizeof(restored));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);

    zassert_ok(
        app_mesh_persistence_test_write_assignment_snapshot(
            &v3, sizeof(v3)));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);
    zassert_equal(restored.epoch, 0u);
    zassert_equal(restored.retired_epoch_count, 0u);

    zassert_ok(
        app_mesh_persistence_test_write_assignment_snapshot(
            &v4, sizeof(v4)));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);
    zassert_equal(restored.epoch, 0u);
    zassert_equal(restored.retired_epoch_count, 0u);

    v4.ordered_epoch_valid = 2u;
    zassert_ok(
        app_mesh_persistence_test_write_assignment_snapshot(
            &v4, sizeof(v4)));
    zassert_equal(
        app_mesh_persistence_restore_discovery_assignment(&restored),
        -EINVAL);
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence,
      test_discovery_assignment_v6_pending_is_retired_fail_closed)
{
    struct legacy_discovery_assignment_snapshot_v6 legacy = {
        .epoch = 700u,
        .table_command_seq = 1700u,
        .table_fingerprint = 0x70001700u,
        .table_packet_seq = 71u,
        .response_spread_ms =
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = 6u,
        .slot = 5u,
        .slot_count = 8u,
        .valid = 1u,
        .ordered_epoch_valid = 1u,
        .ack_pending = 1u,
        .magic = 0x44415336u,
        .size = sizeof(legacy),
    };
    struct app_mesh_discovery_assignment_snapshot restored = {0};

    zassert_equal(sizeof(legacy), 112u);
    legacy.checksum = legacy_discovery_assignment_v6_checksum(&legacy);
    zassert_ok(app_mesh_persistence_test_write_assignment_snapshot(
        &legacy, sizeof(legacy)));
    app_mesh_persistence_test_fail_discovery_assignment_delete(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_restore_discovery_assignment(&restored),
        -EIO);
    zassert_false(restored.valid);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.provisioned);
    zassert_equal(restored.epoch, 0u);
    zassert_equal(restored.slot_count, 0u);
    zassert_false(restored.pending_valid);
    zassert_false(restored.ack_pending);
    memset(&restored, 0xa5, sizeof(restored));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);

    legacy.checksum ^= 1u;
    zassert_ok(app_mesh_persistence_test_write_assignment_snapshot(
        &legacy, sizeof(legacy)));
    zassert_equal(
        app_mesh_persistence_restore_discovery_assignment(&restored),
        -EINVAL);
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence, test_discovery_assignment_v8_integrity_and_read_retry)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {
        .epoch = 277u,
        .table_command_seq = 288u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .slot = 1u,
        .slot_count = 3u,
        .provisioned = true,
        .valid = true,
        .retired_epoch_count = 2u,
        .ordered_epoch_valid = false,
        .retired_epochs = {270u, 260u},
    };
    struct app_mesh_discovery_assignment_snapshot restored;
    struct app_mesh_discovery_assignment_snapshot pristine;
    const size_t corruption_offsets[] = {
        offsetof(struct app_mesh_discovery_assignment_snapshot, epoch),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 table_command_seq),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 table_commitment),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 table_commitment) +
            SEMANTIC_DIGEST_SHA256_LEN - 1u,
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 table_packet_seq),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 response_spread_ms),
        offsetof(struct app_mesh_discovery_assignment_snapshot, local_id),
        offsetof(struct app_mesh_discovery_assignment_snapshot, gateway_id),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_epoch),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_table_command_seq),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_table_commitment),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_table_commitment) +
            SEMANTIC_DIGEST_SHA256_LEN - 1u,
        offsetof(struct app_mesh_discovery_assignment_snapshot, version),
        offsetof(struct app_mesh_discovery_assignment_snapshot, slot),
        offsetof(struct app_mesh_discovery_assignment_snapshot, slot_count),
        offsetof(struct app_mesh_discovery_assignment_snapshot, provisioned),
        offsetof(struct app_mesh_discovery_assignment_snapshot, valid),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 retired_epoch_count),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 ordered_epoch_valid),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 ack_pending),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_slot),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_slot_count),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 pending_valid),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 ack_retry_round),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 retired_epochs),
        offsetof(struct app_mesh_discovery_assignment_snapshot,
                 retired_epochs) +
            (DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP - 1u) *
                sizeof(uint32_t),
        offsetof(struct app_mesh_discovery_assignment_snapshot, magic),
        offsetof(struct app_mesh_discovery_assignment_snapshot, size),
        offsetof(struct app_mesh_discovery_assignment_snapshot, checksum),
    };

    snapshot.table_commitment = test_table_commitment(299u);
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    zassert_ok(
        app_mesh_persistence_test_write_assignment_snapshot(
            &snapshot, sizeof(snapshot)));
    zassert_equal(
        app_mesh_persistence_restore_discovery_assignment(&restored),
        -EINVAL);
    zassert_false(restored.valid);

    snapshot.ordered_epoch_valid = true;
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.ordered_epoch_valid);
    zassert_equal(restored.epoch, snapshot.epoch);
    zassert_equal(restored.magic,
                  APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_MAGIC);
    zassert_equal(restored.size, sizeof(restored));
    zassert_not_equal(restored.checksum, 0u);
    pristine = restored;

    for (size_t i = 0u;
         i < sizeof(corruption_offsets) / sizeof(corruption_offsets[0]);
         i++) {
        struct app_mesh_discovery_assignment_snapshot corrupted = pristine;

        ((uint8_t *)&corrupted)[corruption_offsets[i]] ^= 1u;
        zassert_ok(app_mesh_persistence_test_write_assignment_snapshot(
            &corrupted, sizeof(corrupted)));
        zassert_equal(
            app_mesh_persistence_restore_discovery_assignment(&restored),
            -EINVAL);
        zassert_false(restored.valid);
    }

    snapshot.provisioned = 2u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.provisioned = 0u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.provisioned = 1u;
    snapshot.slot_count = UWB_DISCOVERY_SLOT_COUNT + 1u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.slot_count = 3u;
    snapshot.retired_epochs[0] = snapshot.epoch + 1u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.retired_epochs[0] = 270u;
    snapshot.retired_epochs[1] = 275u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.retired_epochs[1] = 260u;
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));

    app_mesh_persistence_test_fail_discovery_assignment_read(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_restore_discovery_assignment(&restored),
        -EIO);
    zassert_false(restored.valid);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.ordered_epoch_valid);
    zassert_equal(restored.epoch, snapshot.epoch);
    app_mesh_persistence_clear_discovery_assignment();
}

ZTEST(mesh_persistence,
      test_discovery_assignment_delete_failure_preserves_durable_baseline)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {
        .epoch = 377u,
        .table_command_seq = 388u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .slot = 1u,
        .slot_count = 3u,
        .provisioned = true,
        .valid = true,
        .ordered_epoch_valid = true,
    };
    struct app_mesh_discovery_assignment_snapshot restored;

    snapshot.table_commitment = test_table_commitment(399u);
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));
    app_mesh_persistence_test_fail_discovery_assignment_delete(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_clear_discovery_assignment_checked(), -EIO);
    app_mesh_persistence_test_reset_faults();

    /*
     * Initialization must fail closed on that return. The old ordered
     * baseline remains present and must not be mistaken for erased NVS.
     */
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.valid);
    zassert_equal(restored.epoch, snapshot.epoch);
    zassert_equal(restored.table_command_seq, snapshot.table_command_seq);
    zassert_ok(app_mesh_persistence_clear_discovery_assignment_checked());
    memset(&restored, 0, sizeof(restored));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence,
      test_discovery_assignment_pending_keeps_committed_slot_across_reset)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {
        .epoch = 100u,
        .table_command_seq = 1000u,
        .table_packet_seq = 77u,
        .response_spread_ms =
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .pending_epoch = 101u,
        .pending_table_command_seq = 1001u,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .slot = 2u,
        .slot_count = 6u,
        .provisioned = 1u,
        .valid = 1u,
        .ordered_epoch_valid = 1u,
        .ack_pending = 1u,
        .pending_slot = 4u,
        .pending_slot_count = 7u,
        .pending_valid = 1u,
        .ack_retry_round = 9u,
    };
    struct app_mesh_discovery_assignment_snapshot restored = {0};
    struct app_discovery_assignment_policy policy;

    snapshot.table_commitment = test_table_commitment(0x10001000u);
    snapshot.pending_table_commitment =
        test_table_commitment(0x10011001u);
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.provisioned);
    zassert_true(restored.ack_pending);
    zassert_equal(restored.epoch, 100u);
    zassert_equal(restored.slot, 2u);
    zassert_equal(restored.slot_count, 6u);
    zassert_equal(restored.pending_epoch, 101u);
    zassert_equal(restored.pending_table_command_seq, 1001u);
    zassert_mem_equal(&restored.pending_table_commitment,
                      &snapshot.pending_table_commitment,
                      sizeof(restored.pending_table_commitment));
    zassert_equal(restored.pending_slot, 4u);
    zassert_equal(restored.pending_slot_count, 7u);
    zassert_equal(restored.ack_retry_round, 9u);

    app_discovery_assignment_policy_init(
        &policy,
        true,
        restored.ordered_epoch_valid,
        restored.provisioned,
        restored.epoch,
        restored.table_command_seq,
        &restored.table_commitment);
    zassert_true(app_discovery_assignment_policy_restore_pending(
        &policy,
        restored.pending_epoch,
        restored.pending_table_command_seq,
        &restored.pending_table_commitment));
    zassert_true(app_discovery_assignment_policy_normal_click_reply_allowed(
        &policy));
    zassert_equal(policy.committed_epoch, 100u);
    zassert_equal(policy.joining_epoch, 101u);

    snapshot.pending_epoch = snapshot.epoch;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.pending_epoch = 101u;
    snapshot.pending_slot = snapshot.pending_slot_count;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    snapshot.pending_slot = 4u;
    snapshot.ack_pending = 0u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);

    app_mesh_persistence_clear_discovery_assignment();
}

ZTEST(mesh_persistence,
      test_discovery_assignment_first_pending_has_no_committed_slot)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {
        .table_packet_seq = 79u,
        .response_spread_ms =
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .pending_epoch = 201u,
        .pending_table_command_seq = 2001u,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .valid = 1u,
        .ordered_epoch_valid = 1u,
        .ack_pending = 1u,
        .pending_slot = 3u,
        .pending_slot_count = 8u,
        .pending_valid = 1u,
    };
    struct app_mesh_discovery_assignment_snapshot restored = {0};

    snapshot.pending_table_commitment =
        test_table_commitment(0x20012001u);
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.provisioned);
    zassert_equal(restored.epoch, 0u);
    zassert_equal(restored.slot_count, 0u);
    zassert_true(restored.ack_pending);
    zassert_equal(restored.pending_epoch, 201u);
    zassert_equal(restored.pending_slot, 3u);

    app_mesh_persistence_clear_discovery_assignment();
}

ZTEST(mesh_persistence,
      test_discovery_assignment_unlisted_candidate_retains_committed_slot)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {
        .epoch = 300u,
        .table_command_seq = 3000u,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .pending_epoch = 301u,
        .pending_table_command_seq = 3001u,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .slot = 2u,
        .slot_count = 6u,
        .provisioned = 1u,
        .valid = 1u,
        .ordered_epoch_valid = 1u,
        .pending_slot = 0u,
        .pending_slot_count = 7u,
        .pending_valid = 1u,
    };
    struct app_mesh_discovery_assignment_snapshot restored = {0};
    struct app_discovery_assignment_policy policy;

    snapshot.table_commitment = test_table_commitment(0x30003000u);
    snapshot.pending_table_commitment =
        test_table_commitment(0x30013001u);
    zassert_ok(app_mesh_persistence_save_discovery_assignment(&snapshot));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.provisioned);
    zassert_true(restored.pending_valid);
    zassert_false(restored.ack_pending);
    zassert_equal(restored.slot, 2u);
    zassert_equal(restored.pending_epoch, 301u);

    app_discovery_assignment_policy_init(
        &policy,
        true,
        true,
        true,
        restored.epoch,
        restored.table_command_seq,
        &restored.table_commitment);
    zassert_true(app_discovery_assignment_policy_restore_pending(
        &policy,
        restored.pending_epoch,
        restored.pending_table_command_seq,
        &restored.pending_table_commitment));
    zassert_true(app_discovery_assignment_policy_normal_click_reply_allowed(
        &policy));

    snapshot.pending_slot = 1u;
    zassert_equal(
        app_mesh_persistence_save_discovery_assignment(&snapshot),
        -EINVAL);
    app_mesh_persistence_clear_discovery_assignment();
}

ZTEST(mesh_persistence, test_gateway_assignment_epoch_round_trip_and_write_failure)
{
    uint32_t epoch = UINT32_MAX;
    int ret;

    zassert_ok(app_mesh_persistence_init());
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);

    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(UINT32_MAX));
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, UINT32_MAX);

    app_mesh_persistence_test_fail_gateway_assignment_epoch_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_gateway_assignment_epoch(1u),
                  -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, UINT32_MAX);

    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(1u));
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 1u);
    zassert_ok(app_mesh_persistence_test_delete_gateway_assignment_epoch());
}

ZTEST(mesh_persistence, test_gateway_assignment_epoch_corruption_fails_closed)
{
    struct app_mesh_gateway_assignment_epoch_snapshot snapshot = {
        .magic = APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_MAGIC,
        .epoch = 77u,
        .version = APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .valid = 1u,
    };
    uint8_t truncated = 0xa5u;
    uint32_t epoch = UINT32_MAX;
    int ret;

    zassert_ok(app_mesh_persistence_init());
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);
    snapshot.checksum = 0u;
    snapshot.checksum =
        proto_crc16_ccitt_false((const uint8_t *)&snapshot,
                                sizeof(snapshot));
    snapshot.checksum ^= 1u;
    zassert_ok(
        app_mesh_persistence_test_write_gateway_assignment_epoch_snapshot(
            &snapshot, sizeof(snapshot)));
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_epoch(&epoch),
        -EINVAL);
    zassert_equal(epoch, 0u);

    zassert_ok(
        app_mesh_persistence_test_write_gateway_assignment_epoch_snapshot(
            &truncated, sizeof(truncated)));
    epoch = UINT32_MAX;
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_epoch(&epoch),
        -EINVAL);
    zassert_equal(epoch, 0u);
    zassert_ok(app_mesh_persistence_test_delete_gateway_assignment_epoch());
}

static void finalize_survey_generation_snapshot(
    struct app_mesh_survey_generation_snapshot *snapshot)
{
    snapshot->checksum = 0u;
    snapshot->checksum =
        proto_crc16_ccitt_false((const uint8_t *)snapshot,
                                sizeof(*snapshot));
}

ZTEST(mesh_persistence,
      test_gateway_survey_generation_reservation_and_corruption)
{
    struct app_mesh_survey_generation_snapshot snapshot = {
        .magic = APP_MESH_SURVEY_GENERATION_SNAPSHOT_MAGIC,
        .local_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .generation = UINT32_MAX,
        .version = APP_MESH_SURVEY_GENERATION_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .role = DEVICE_ROLE,
        .valid = 1u,
    };
    uint64_t generation = UINT64_MAX;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_test_delete_survey_generation_snapshot());

    zassert_ok(app_mesh_persistence_reserve_gateway_survey_generation(
        GATEWAY_ID, &generation));
    zassert_equal(generation, 1u);
    zassert_ok(app_mesh_persistence_reserve_gateway_survey_generation(
        GATEWAY_ID, &generation));
    zassert_equal(generation, 2u);

    finalize_survey_generation_snapshot(&snapshot);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    zassert_ok(app_mesh_persistence_reserve_gateway_survey_generation(
        GATEWAY_ID, &generation));
    zassert_equal(generation, UINT64_C(0x100000001));

    snapshot.generation = UINT64_MAX;
    finalize_survey_generation_snapshot(&snapshot);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_reserve_gateway_survey_generation(
            GATEWAY_ID, &generation),
        -EOVERFLOW);
    zassert_equal(generation, 0u);

    snapshot.generation = 7u;
    snapshot.role = (uint8_t)(DEVICE_ROLE - 1u);
    finalize_survey_generation_snapshot(&snapshot);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_reserve_gateway_survey_generation(
            GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    snapshot.role = DEVICE_ROLE;
    snapshot.local_id ^= 1u;
    finalize_survey_generation_snapshot(&snapshot);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_reserve_gateway_survey_generation(
            GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    snapshot.local_id = GATEWAY_ID;
    finalize_survey_generation_snapshot(&snapshot);
    snapshot.checksum ^= 1u;
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_reserve_gateway_survey_generation(
            GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    zassert_ok(app_mesh_persistence_test_delete_survey_generation_snapshot());
}

ZTEST(mesh_persistence,
      test_gateway_assignment_epoch_reconciles_cursor_and_membership_proof)
{
    struct gateway_membership_roster roster = make_gateway_membership_roster();
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0xceu, 0x11u, 0x5au, 0x39u},
    };
    uint32_t epoch = UINT32_MAX;
    uint32_t baseline = UINT32_MAX;
    int ret;

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);

    /* Two positively absent records establish the explicit initial cursor. */
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);
    baseline = UINT32_MAX;
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(&baseline),
        0);
    zassert_equal(baseline, 0u);
    epoch = UINT32_MAX;
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);

    /* A proof-only reset gap repairs the cursor before recovery succeeds. */
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster, 77u, 1001u, &table_commitment, NULL));
    baseline = 0u;
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(&baseline),
        1);
    zassert_equal(baseline, 77u);
    epoch = UINT32_MAX;
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 77u);
    epoch = 0u;
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 77u);

    /* Equal records require no write, so an injected write fault is inert. */
    app_mesh_persistence_test_fail_gateway_assignment_epoch_write(-EIO, 1u);
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 77u);
    app_mesh_persistence_test_reset_faults();

    /* A newer proof repairs upward; a newer reservation remains untouched. */
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster, 78u, 1003u, &table_commitment, NULL));
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 78u);
    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(79u));
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 79u);

    /* RFC 1982 ordering remains correct at UINT32 wrap in both directions. */
    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(UINT32_MAX));
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster, 1u, 1005u, &table_commitment, NULL));
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 1u);
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster, UINT32_MAX, 1007u, &table_commitment, NULL));
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 1u);

    /*
     * A half-range split has no RFC 1982 order. Preserve both records and
     * refuse to select a baseline.
     */
    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(1u));
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster,
        UINT32_C(0x80000001),
        1009u,
        &table_commitment,
        NULL));
    epoch = UINT32_MAX;
    zassert_equal(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch),
        -ESTALE);
    zassert_equal(epoch, 0u);
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 1u);

    /* A cursor-only state keeps the already reserved high-water mark. */
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(55u));
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 55u);

    /* A valid roster without assignment proof contributes no guessed epoch. */
    zassert_ok(app_mesh_persistence_save_gateway_membership(&roster));
    zassert_ok(app_mesh_persistence_test_delete_gateway_assignment_epoch());
    baseline = UINT32_MAX;
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(&baseline),
        0);
    zassert_equal(baseline, 0u);
    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);

    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);
}

ZTEST(mesh_persistence,
      test_gateway_assignment_proof_cursor_repair_failure_is_retryable)
{
    struct gateway_membership_roster roster = make_gateway_membership_roster();
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0x90u, 0x2du, 0x71u, 0xb4u},
    };
    uint32_t epoch = UINT32_MAX;
    int ret;

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster, 900u, 901u, &table_commitment, NULL));

    app_mesh_persistence_test_fail_gateway_assignment_epoch_write(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch),
        -EIO);
    zassert_equal(epoch, 0u);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);

    zassert_ok(
        app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 900u);
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 900u);

    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_test_delete_gateway_assignment_epoch());
}

ZTEST(mesh_persistence, test_assignment_keys_survive_cross_role_reflash)
{
    const struct app_mesh_discovery_assignment_snapshot anchor_snapshot = {
        .epoch = 277u,
        .table_command_seq = 288u,
        .table_commitment = {
            .bytes = {0x2bu, 0xbau, 0x41u, 0x99u},
        },
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
        .slot = 1u,
        .slot_count = 3u,
        .provisioned = true,
        .valid = true,
        .ordered_epoch_valid = true,
    };
    struct app_mesh_discovery_assignment_snapshot restored = {0};
    uint32_t epoch = UINT32_MAX;
    int ret;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_discovery_assignment();
    ret = app_mesh_persistence_test_delete_gateway_assignment_epoch();
    zassert_true(ret == 0 || ret == -ENOENT);

    /* A preserved anchor record must look absent, not corrupt, to a gateway. */
    zassert_ok(
        app_mesh_persistence_save_discovery_assignment(&anchor_snapshot));
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 0u);

    /* The gateway cursor must coexist without replacing the anchor record. */
    zassert_ok(app_mesh_persistence_save_gateway_assignment_epoch(300u));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_true(restored.valid);
    zassert_equal(restored.epoch, anchor_snapshot.epoch);

    /* With no anchor record, a preserved gateway cursor is safely ignored. */
    app_mesh_persistence_clear_discovery_assignment();
    memset(&restored, 0xa5, sizeof(restored));
    zassert_ok(
        app_mesh_persistence_restore_discovery_assignment(&restored));
    zassert_false(restored.valid);
    zassert_ok(app_mesh_persistence_restore_gateway_assignment_epoch(&epoch));
    zassert_equal(epoch, 300u);
    zassert_ok(app_mesh_persistence_test_delete_gateway_assignment_epoch());
}

struct preempt_save_ctx {
    struct mesh_relay *relay;
    uint32_t now_ms;
    uint8_t save_count;
    uint8_t schedule_count;
};

struct result_grant_send_ctx {
    struct mesh_relay *relay;
    uint32_t now_ms;
    int send_ret;
    uint8_t save_count;
    uint8_t send_count;
    uint8_t note_count;
};

static void timeout_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

static int save_deferred_outbox_for_preempt(void *opaque)
{
    struct preempt_save_ctx *ctx = opaque;

    ctx->save_count++;
    return app_mesh_persistence_save_deferred_outbox(ctx->relay, ctx->now_ms);
}

static int schedule_timeout_for_preempt(void *opaque)
{
    struct preempt_save_ctx *ctx = opaque;

    ctx->schedule_count++;
    return k_work_reschedule(&test_tx_timeout_work, K_MSEC(1000));
}

static int cancel_active_tx_for_preempt(void *opaque)
{
    struct preempt_save_ctx *ctx = opaque;

    mesh_relay_cancel_tx(ctx->relay);
    return 0;
}

static int save_child_custody_for_grant(void *opaque)
{
    struct result_grant_send_ctx *ctx = opaque;

    ctx->save_count++;
    return app_mesh_persistence_save_child_custody(ctx->relay, ctx->now_ms);
}

static int send_result_grant_for_test(const struct mesh_outbound *out,
                                      void *opaque)
{
    struct result_grant_send_ctx *ctx = opaque;

    ARG_UNUSED(out);

    ctx->send_count++;
    return ctx->send_ret;
}

static void note_tx_sent_for_grant(const struct mesh_outbound *out,
                                   void *opaque)
{
    struct result_grant_send_ctx *ctx = opaque;

    ARG_UNUSED(out);

    ctx->note_count++;
}

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return (result->actions & action) != 0u;
}

static struct route_candidate direct_gateway_route(uint8_t quality)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 13u,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = quality,
        .failure_count = 0u,
        .valid = true,
    };
}

static void assert_result_id_equal(const struct command_result_id *actual,
                                   const struct command_result_id *expected)
{
    zassert_equal(actual->gateway_id, expected->gateway_id);
    zassert_equal(actual->gateway_epoch, expected->gateway_epoch);
    zassert_equal(actual->command_seq, expected->command_seq);
    zassert_equal(actual->node_id, expected->node_id);
    zassert_equal(actual->node_boot_counter, expected->node_boot_counter);
    zassert_equal(actual->result_seq, expected->result_seq);
}

static void assert_collection_result_id_equal(
    const struct gateway_collection_state *collection,
    const struct gateway_collection_result_id *actual,
    const struct command_result_id *expected)
{
    zassert_equal(collection->gateway_id, expected->gateway_id);
    zassert_equal(collection->gateway_epoch, expected->gateway_epoch);
    zassert_equal(collection->command_seq, expected->command_seq);
    zassert_equal(actual->node_id, expected->node_id);
    zassert_equal(actual->node_boot_counter, expected->node_boot_counter);
    zassert_equal(actual->result_seq, expected->result_seq);
}

ZTEST(mesh_persistence, test_gateway_click_journal_round_trip_after_reset)
{
    const uint8_t payload[] = { 0x01u, 0x02u, 0x03u, 0x04u, 0x05u };
    struct proto_packet packet = make_gateway_click_packet(
        0x2201u, sizeof(payload));
    struct proto_packet restored_packet;
    uint8_t restored_payload[PACKET_MAX_PAYLOAD_LEN];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 321u));

    /* The caller-owned buffers model a fresh gateway process after reset. */
    memset(&restored_packet, 0, sizeof(restored_packet));
    memset(restored_payload, 0, sizeof(restored_payload));
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    zassert_equal(restored_packet.msg_type, packet.msg_type);
    zassert_equal(restored_packet.flags, packet.flags);
    zassert_equal(restored_packet.src_id, packet.src_id);
    zassert_equal(restored_packet.dst_id, packet.dst_id);
    zassert_equal(restored_packet.session_id, packet.session_id);
    zassert_equal(restored_packet.seq, packet.seq);
    zassert_equal(restored_packet.ttl, packet.ttl);
    zassert_equal(restored_packet.payload_len, packet.payload_len);
    zassert_equal(restored_packet.message_age_ms, packet.message_age_ms);
    zassert_equal(restored_len, sizeof(payload));
    zassert_mem_equal(restored_payload, payload, sizeof(payload));
    zassert_equal(restored_at_ms, 321u);

    zassert_ok(app_mesh_persistence_clear_gateway_click_journal_if_matches(
        &restored_packet, restored_payload, restored_len));
    restored_len = 0u;
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 0);
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_reset_after_ack_before_notify_replays_every_class)
{
    static const uint8_t message_types[] = {
        MSG_CLICK_REPORT,
        MSG_COMMAND_RESULT,
        MSG_RESULT_BUNDLE,
        MSG_SURVEY_DISCOVERY_REPORT,
        MSG_SURVEY_PAIR_RESULT,
    };
    const uint8_t payload[] = { 0x23u, 0x45u, 0x67u, 0x89u, 0xabu, 0xcdu };
    uint8_t restored_payload[sizeof(payload)];
    struct proto_packet restored_packet;
    size_t restored_len;
    uint32_t restored_at_ms;

    zassert_ok(app_mesh_persistence_init());
    for (size_t i = 0u; i < ARRAY_SIZE(message_types); i++) {
        struct proto_packet packet = make_gateway_host_packet(
            message_types[i], (uint16_t)(0x2300u + i), sizeof(payload));
        uint32_t accepted_at_ms = 700u + (uint32_t)i;

        zassert_true(
            app_mesh_persistence_gateway_host_journal_supports(&packet));
        zassert_ok(app_mesh_persistence_clear_gateway_host_journal());

        /*
         * Saving models the durable admission immediately before the gateway
         * ACK.  Reinitializing the caller-owned buffers models reset before
         * the BLE notification reaches its terminal completion callback.
         */
        zassert_ok(app_mesh_persistence_save_gateway_host_journal(
            &packet, payload, sizeof(payload), accepted_at_ms));
        memset(&restored_packet, 0, sizeof(restored_packet));
        memset(restored_payload, 0, sizeof(restored_payload));
        restored_len = 0u;
        restored_at_ms = 0u;
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms), 1);
        zassert_equal(restored_packet.msg_type, packet.msg_type);
        zassert_equal(restored_packet.flags, packet.flags);
        zassert_equal(restored_packet.src_id, packet.src_id);
        zassert_equal(restored_packet.dst_id, packet.dst_id);
        zassert_equal(restored_packet.session_id, packet.session_id);
        zassert_equal(restored_packet.seq, packet.seq);
        zassert_equal(restored_packet.ttl, packet.ttl);
        zassert_equal(restored_packet.payload_len, packet.payload_len);
        zassert_equal(restored_packet.message_age_ms, packet.message_age_ms);
        zassert_equal(restored_len, sizeof(payload));
        zassert_mem_equal(restored_payload, payload, sizeof(payload));
        zassert_equal(restored_at_ms, accepted_at_ms);

        /* Terminal BLE completion retires only the matching durable owner. */
        zassert_ok(
            app_mesh_persistence_clear_gateway_host_journal_if_matches(
                &restored_packet, restored_payload, restored_len));
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms), 0);
    }
}

ZTEST(mesh_persistence,
      test_gateway_bundle_projection_metadata_survives_reset)
{
    const uint8_t raw_payload[] = {
        0xa1u, 0x02u, 0x11u, 0x22u, 0xb2u, 0x01u, 0x33u,
    };
    struct proto_packet packet = make_gateway_host_packet(
        MSG_RESULT_BUNDLE, 0x2340u, sizeof(raw_payload));
    struct proto_packet restored_packet;
    uint8_t restored_payload[sizeof(raw_payload)];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;
    uint8_t restored_projection_mask = 0u;
    uint8_t matched_projection_mask = 0u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_prepare_gateway_host_journal_projection(
        &packet,
        raw_payload,
        sizeof(raw_payload),
        777u,
        0x02u));
    zassert_equal(
        app_mesh_persistence_gateway_host_journal_matches_with_projection(
            &packet,
            raw_payload,
            sizeof(raw_payload),
            &matched_projection_mask),
        APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED);
    zassert_equal(matched_projection_mask, 0x02u);

    /* The projection is part of the immutable journal owner identity. */
    zassert_equal(app_mesh_persistence_prepare_gateway_host_journal_projection(
                      &packet,
                      raw_payload,
                      sizeof(raw_payload),
                      778u,
                      0x01u),
                  -EBUSY);
    zassert_ok(app_mesh_persistence_commit_gateway_host_journal(
        &packet, raw_payload, sizeof(raw_payload)));

    /* Fresh caller-owned state models restore after the ACK/reset window. */
    memset(&restored_packet, 0, sizeof(restored_packet));
    memset(restored_payload, 0, sizeof(restored_payload));
    zassert_equal(
        app_mesh_persistence_restore_gateway_host_journal_projection(
            &restored_packet,
            restored_payload,
            sizeof(restored_payload),
            &restored_len,
            &restored_at_ms,
            &restored_projection_mask),
        APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);
    zassert_equal(restored_projection_mask, 0x02u);
    zassert_equal(restored_at_ms, 777u);
    zassert_equal(restored_len, sizeof(raw_payload));
    zassert_mem_equal(restored_payload, raw_payload, sizeof(raw_payload));
    zassert_equal(restored_packet.msg_type, packet.msg_type);
    zassert_equal(restored_packet.src_id, packet.src_id);
    zassert_equal(restored_packet.dst_id, packet.dst_id);
    zassert_equal(restored_packet.session_id, packet.session_id);
    zassert_equal(restored_packet.seq, packet.seq);
    zassert_equal(restored_packet.payload_len, sizeof(raw_payload));

    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &restored_packet, restored_payload, restored_len));
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_notified_phase_is_durable_and_idempotent)
{
    const uint8_t raw_payload[] = {
        0xa1u, 0x02u, 0x11u, 0x22u, 0xb2u, 0x01u, 0x33u,
    };
    struct proto_packet packet = make_gateway_host_packet(
        MSG_RESULT_BUNDLE, 0x2341u, sizeof(raw_payload));
    struct proto_packet wrong_packet = packet;
    struct proto_packet restored_packet;
    uint8_t restored_payload[sizeof(raw_payload)];
    uint8_t raw_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;
    uint8_t restored_projection_mask = 0u;

    test_payload_digest(raw_payload,
                        sizeof(raw_payload),
                        raw_payload_digest);
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_prepare_gateway_host_journal_projection(
        &packet,
        raw_payload,
        sizeof(raw_payload),
        778u,
        0x02u));

    /* PREPARED has not reached the host and must never become NOTIFIED. */
    zassert_equal(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, raw_payload_digest),
        -EBADMSG);
    zassert_ok(app_mesh_persistence_commit_gateway_host_journal(
        &packet, raw_payload, sizeof(raw_payload)));

    wrong_packet.seq++;
    zassert_equal(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &wrong_packet, raw_payload_digest),
        -ESTALE);
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, raw_payload_digest));

    memset(&restored_packet, 0, sizeof(restored_packet));
    memset(restored_payload, 0, sizeof(restored_payload));
    zassert_equal(
        app_mesh_persistence_restore_gateway_host_journal_projection(
            &restored_packet,
            restored_payload,
            sizeof(restored_payload),
            &restored_len,
            &restored_at_ms,
            &restored_projection_mask),
        APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED);
    zassert_equal(restored_projection_mask, 0x02u);
    zassert_equal(restored_at_ms, 778u);
    zassert_equal(restored_len, sizeof(raw_payload));
    zassert_mem_equal(restored_payload, raw_payload, sizeof(raw_payload));
    zassert_equal(restored_packet.session_id, packet.session_id);
    zassert_equal(restored_packet.seq, packet.seq);

    /* Reset/retry of the same terminal marker never regresses its phase. */
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, raw_payload_digest));
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &packet, raw_payload, sizeof(raw_payload)));
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_notified_verify_failure_recovers_exact_phase)
{
    const uint8_t payload[] = { 0x91u, 0x82u, 0x73u, 0x64u };
    struct proto_packet packet = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 0x2342u, sizeof(payload));
    struct proto_packet restored_packet;
    uint8_t restored_payload[sizeof(payload)];
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    test_payload_digest(payload, sizeof(payload), payload_digest);
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_prepare_gateway_host_journal(
        &packet, payload, sizeof(payload), 779u));
    zassert_ok(app_mesh_persistence_commit_gateway_host_journal(
        &packet, payload, sizeof(payload)));

    /*
     * The NOTIFIED marker can be durable even when its verification read
     * fails.  Exact retry must recognize that terminal phase and must not
     * expose the journal as COMMITTED host work again.
     */
    app_mesh_persistence_test_fail_gateway_click_metadata_verify(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest),
        -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED);
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest));
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &packet, payload, sizeof(payload)));
}

ZTEST(mesh_persistence,
      test_gateway_terminal_receipt_exact_conflict_and_eack_exclusion)
{
    uint8_t ordinary_payload[32];
    uint8_t collection_payload[128];
    uint8_t changed_payload[32];
    size_t ordinary_len =
        build_ordinary_command_result(ordinary_payload,
                                      sizeof(ordinary_payload));
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 1701u,
        .node_id = CHILD_ID,
        .node_boot_counter = 71u,
        .result_seq = 9u,
    };
    size_t collection_len =
        build_collection_command_result(collection_payload,
                                        sizeof(collection_payload),
                                        &result_id,
                                        2701u,
                                        0u);
    struct proto_packet packet = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 0x2401u, (uint16_t)ordinary_len);
    struct proto_packet bundle = make_gateway_host_packet(
        MSG_RESULT_BUNDLE, 0x2402u, 1u);
    struct proto_packet collection =
        make_collection_result_packet(&result_id,
                                      (uint16_t)collection_len);
    struct proto_packet transport_retry = packet;
    struct proto_packet changed_flags = packet;

    memcpy(changed_payload, ordinary_payload, ordinary_len);
    changed_payload[ordinary_len - 1u] ^= 0x01u;
    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();

    zassert_true(app_gateway_terminal_receipts_supports(
        &packet, ordinary_payload, ordinary_len));
    zassert_false(app_gateway_terminal_receipts_supports(
        &bundle, (const uint8_t[]){0u}, 1u));
    zassert_false(app_gateway_terminal_receipts_supports(
        &collection, collection_payload, collection_len));

    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, ordinary_payload, ordinary_len, 1000u),
                  1);
    zassert_ok(app_gateway_terminal_receipts_record(
        &packet, ordinary_payload, ordinary_len, 1001u));
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, ordinary_payload, ordinary_len, 1002u),
                  1);

    /* TTL and age are relay-local retry fields, not host semantic identity. */
    transport_retry.ttl = 1u;
    transport_retry.message_age_ms = 9000u;
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &transport_retry,
                      ordinary_payload,
                      ordinary_len,
                      1003u),
                  1);
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, changed_payload, ordinary_len, 1004u),
                  -EBADMSG);
    changed_flags.flags |= FLAG_DIAGNOSTIC;
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &changed_flags,
                      ordinary_payload,
                      ordinary_len,
                      1005u),
                  -EBADMSG);

    clear_terminal_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_terminal_receipts_cover_all_sources_and_reuse_confirmed_slot)
{
    const uint8_t payload[] = {0x51u, 0x62u, 0x73u};
    struct proto_packet packet;
    struct proto_packet confirmed;
    struct proto_packet overflow;
    struct proto_packet confirm;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t confirm_payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();
    for (uint8_t i = 0u;
         i < APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY;
         i++) {
        packet = make_gateway_click_packet(
            (uint16_t)(0x2500u + i), sizeof(payload));
        packet.src_id = UINT64_C(0x1000000000000000) + i + 1u;
        zassert_equal(app_gateway_terminal_receipts_record(
                          &packet, payload, sizeof(payload), 2000u + i),
                      1);
    }

    overflow = make_gateway_click_packet(0x2600u, sizeof(payload));
    overflow.src_id = UINT64_C(0x2000000000000001);
    zassert_equal(app_gateway_terminal_receipts_record(
                      &overflow, payload, sizeof(payload), 2100u),
                  -ENOSPC);

    confirmed = make_gateway_click_packet(0x2511u, sizeof(payload));
    confirmed.src_id = UINT64_C(0x1000000000000000) + 18u;
    make_gateway_ack_confirm(&confirmed,
                             payload,
                             sizeof(payload),
                             &confirm,
                             confirm_payload,
                             &confirm_payload_len);
    zassert_equal(app_gateway_terminal_receipts_confirm(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len,
                      2101u),
                  1);
    zassert_equal(app_gateway_terminal_receipts_record(
                      &overflow, payload, sizeof(payload), 2102u),
                  1);
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &confirmed, payload, sizeof(payload), 2103u),
                  0);
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &overflow, payload, sizeof(payload), 2103u),
                  1);

    clear_terminal_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_terminal_receipt_reset_expiry_and_storage_failures)
{
    const uint8_t payload[] = {0xa1u, 0xb2u, 0xc3u, 0xd4u};
    const uint8_t corrupt[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
    struct proto_packet packet =
        make_gateway_click_packet(0x2701u, sizeof(payload));
    struct proto_packet confirm;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t confirm_payload_len = 0u;
    uint32_t restore_at_ms = 5000u;

    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();
    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, payload, sizeof(payload), 100u),
                  1);

    /* Reset restarts the complete source raw-custody horizon. */
    app_gateway_terminal_receipts_test_reset_runtime();
    zassert_ok(app_gateway_terminal_receipts_restore(restore_at_ms));
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet,
                      payload,
                      sizeof(payload),
                      restore_at_ms +
                          APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS),
                  1);

    app_mesh_persistence_test_fail_gateway_terminal_receipt_delete(
        -EIO, 1u);
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet,
                      payload,
                      sizeof(payload),
                      restore_at_ms +
                          APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS + 1u),
                  -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet,
                      payload,
                      sizeof(payload),
                      restore_at_ms +
                          APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS + 1u),
                  0);

    app_mesh_persistence_test_fail_gateway_terminal_receipt_write(
        -EIO, 1u);
    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, payload, sizeof(payload), 6000u),
                  -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, payload, sizeof(payload), 6001u),
                  0);
    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, payload, sizeof(payload), 6002u),
                  1);

    app_gateway_terminal_receipts_test_reset_runtime();
    app_mesh_persistence_test_fail_gateway_terminal_receipt_read(
        -EIO, 1u);
    zassert_equal(app_gateway_terminal_receipts_restore(6003u), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_gateway_terminal_receipts_restore(6004u));

    make_gateway_ack_confirm(&packet,
                             payload,
                             sizeof(payload),
                             &confirm,
                             confirm_payload,
                             &confirm_payload_len);
    app_mesh_persistence_test_fail_gateway_terminal_receipt_delete(
        -EIO, 1u);
    zassert_equal(app_gateway_terminal_receipts_confirm(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len,
                      6005u),
                  -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, payload, sizeof(payload), 6006u),
                  1);
    zassert_equal(app_gateway_terminal_receipts_confirm(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len,
                      6007u),
                  1);

    clear_terminal_receipts();
    zassert_ok(app_mesh_persistence_test_write_gateway_terminal_receipt_raw(
        0u, corrupt, sizeof(corrupt)));
    app_gateway_terminal_receipts_test_reset_runtime();
    zassert_equal(app_gateway_terminal_receipts_restore(7000u), -EBADMSG);
    zassert_equal(app_gateway_terminal_receipts_restore(7001u), -EBADMSG);
    zassert_ok(
        app_mesh_persistence_test_delete_gateway_terminal_receipt(0u));
    app_gateway_terminal_receipts_test_reset_runtime();
    zassert_ok(app_gateway_terminal_receipts_restore(7002u));
}

ZTEST(mesh_persistence,
      test_gateway_terminal_receipt_crash_cuts_and_lost_confirm_ack)
{
    const uint8_t payload[] = {0x11u, 0x22u, 0x33u, 0x44u};
    struct proto_packet packet =
        make_gateway_click_packet(0x2801u, sizeof(payload));
    struct proto_packet confirm;
    struct proto_packet restored_packet;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    uint8_t restored_payload[sizeof(payload)];
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t confirm_payload_len = 0u;
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    test_payload_digest(payload, sizeof(payload), payload_digest);
    make_gateway_ack_confirm(&packet,
                             payload,
                             sizeof(payload),
                             &confirm,
                             confirm_payload,
                             &confirm_payload_len);
    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());

    /* Crash after NOTIFIED but before receipt: journal alone stays terminal
     * and raw duplicate suppression is not fabricated. */
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &packet, payload, sizeof(payload), 8000u));
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest));
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, payload, sizeof(payload), 8001u),
                  0);
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED);

    /* Crash after receipt but before journal clear: reset preserves the exact
     * terminal identity, so raw replay is ACKed without a second host emit. */
    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, payload, sizeof(payload), 8002u),
                  1);
    app_gateway_terminal_receipts_test_reset_runtime();
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, payload, sizeof(payload), 8003u),
                  1);
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &packet, payload, sizeof(payload)));
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  0);

    /* ACK_CONFIRM after NOTIFIED consumes both proofs. Losing the returned
     * gateway ACK is harmless because an exact retry is statelessly ACKable. */
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &packet, payload, sizeof(payload), 8010u));
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest));
    zassert_ok(app_gateway_terminal_receipts_record(
        &packet, payload, sizeof(payload), 8011u));
    zassert_equal(app_gateway_terminal_receipts_confirm(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len,
                      8012u),
                  1);
    zassert_equal(app_mesh_persistence_confirm_gateway_host_journal(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len),
                  2);
    zassert_ok(app_gateway_terminal_receipts_confirm(
        &confirm, confirm_payload, confirm_payload_len, 8013u));
    zassert_ok(app_mesh_persistence_confirm_gateway_host_journal(
        &confirm, confirm_payload, confirm_payload_len));
}

ZTEST(mesh_persistence,
      test_gateway_source_confirm_before_notified_needs_no_receipt)
{
    const uint8_t payload[] = {0x71u, 0x72u, 0x73u};
    struct proto_packet packet =
        make_gateway_click_packet(0x2810u, sizeof(payload));
    struct proto_packet confirm;
    struct proto_packet restored_packet;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    uint8_t restored_payload[sizeof(payload)];
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t confirm_payload_len = 0u;
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    test_payload_digest(payload, sizeof(payload), payload_digest);
    make_gateway_ack_confirm(&packet,
                             payload,
                             sizeof(payload),
                             &confirm,
                             confirm_payload,
                             &confirm_payload_len);
    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &packet, payload, sizeof(payload), 8100u));

    zassert_equal(app_mesh_persistence_confirm_gateway_host_journal(
                      &confirm,
                      confirm_payload,
                      confirm_payload_len),
                  1);
    zassert_equal(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest),
        1);
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  0);
    zassert_equal(app_gateway_terminal_receipts_classify(
                      &packet, payload, sizeof(payload), 8101u),
                  0);
}

ZTEST(mesh_persistence,
      test_gateway_notified_torn_clear_requires_terminal_proof)
{
    const uint8_t payload[] = {0x91u, 0x92u, 0x93u, 0x94u};
    struct proto_packet packet =
        make_gateway_click_packet(0x2820u, sizeof(payload));
    struct proto_packet marker_packet;
    struct proto_packet restored_packet;
    uint8_t marker_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t restored_payload[sizeof(payload)];
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;
    bool source_confirmed = false;

    test_payload_digest(payload, sizeof(payload), payload_digest);
    zassert_ok(app_mesh_persistence_init());
    clear_terminal_receipts();
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &packet, payload, sizeof(payload), 8200u));
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest));

    /* A one-shot metadata-delete failure leaves NOTIFIED plus no payload. */
    app_mesh_persistence_test_fail_gateway_click_metadata_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_gateway_host_journal_if_matches(
                      &packet, payload, sizeof(payload)),
                  -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  -EBADMSG);
    zassert_equal(
        app_mesh_persistence_restore_gateway_host_terminal_marker(
            &marker_packet, marker_digest, &source_confirmed),
        1);
    zassert_false(source_confirmed);
    zassert_equal(app_gateway_terminal_receipts_classify_identity(
                      &marker_packet, marker_digest, 8201u),
                  0);

    /* Without receipt/source proof the marker remains fail-closed. Repair the
     * exact payload, create the receipt, and retry the same clear safely. */
    zassert_ok(app_mesh_persistence_prepare_gateway_host_journal(
        &packet, payload, sizeof(payload), 8202u));
    zassert_equal(app_gateway_terminal_receipts_record(
                      &packet, payload, sizeof(payload), 8203u),
                  1);
    zassert_equal(app_gateway_terminal_receipts_classify_identity(
                      &marker_packet, marker_digest, 8204u),
                  1);
    zassert_equal(
        app_mesh_persistence_retire_notified_gateway_host_journal_if_matches(
            &marker_packet, marker_digest),
        1);
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  0);
    clear_terminal_receipts();
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_phase_crash_windows_for_every_class)
{
    static const uint8_t message_types[] = {
        MSG_CLICK_REPORT,
        MSG_COMMAND_RESULT,
        MSG_RESULT_BUNDLE,
        MSG_SURVEY_DISCOVERY_REPORT,
        MSG_SURVEY_PAIR_RESULT,
    };
    const uint8_t payload[] = { 0x33u, 0x44u, 0x55u, 0x66u };
    uint8_t restored_payload[sizeof(payload)];
    struct proto_packet restored_packet;
    size_t restored_len;
    uint32_t restored_at_ms;

    zassert_ok(app_mesh_persistence_init());
    for (size_t i = 0u; i < ARRAY_SIZE(message_types); i++) {
        struct proto_packet packet = make_gateway_host_packet(
            message_types[i], (uint16_t)(0x2350u + i), sizeof(payload));

        zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms), 0);

        /* Reset before semantic mutation observes PREPARED, never committed. */
        zassert_ok(app_mesh_persistence_prepare_gateway_host_journal(
            &packet, payload, sizeof(payload), 1000u + (uint32_t)i));
        zassert_equal(app_mesh_persistence_gateway_host_journal_matches(
                          &packet, payload, sizeof(payload)),
                      APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED);
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms),
                      APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED);

        /* Semantic success promotes the same exact owner atomically. */
        zassert_ok(app_mesh_persistence_commit_gateway_host_journal(
            &packet, payload, sizeof(payload)));
        zassert_equal(app_mesh_persistence_gateway_host_journal_matches(
                          &packet, payload, sizeof(payload)),
                      APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms),
                      APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);

        /*
         * Volatile orchestration abandoned by reset is retained under an
         * explicit raw-recovery phase, never mislabeled semantic commit.
         */
        zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
        zassert_ok(app_mesh_persistence_prepare_gateway_host_journal(
            &packet, payload, sizeof(payload), 1100u + (uint32_t)i));
        zassert_ok(app_mesh_persistence_recover_raw_gateway_host_journal(
            &packet, payload, sizeof(payload)));
        zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                          &restored_packet,
                          restored_payload,
                          sizeof(restored_payload),
                          &restored_len,
                          &restored_at_ms),
                      APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW);
    }
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_phase_faults_are_idempotent)
{
    const uint8_t payload[] = { 0x91u, 0x82u, 0x73u, 0x64u };
    struct proto_packet packet = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 0x2360u, sizeof(payload));
    struct proto_packet restored_packet;
    uint8_t restored_payload[sizeof(payload)];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());

    app_mesh_persistence_test_fail_gateway_click_metadata_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_prepare_gateway_host_journal(
                      &packet, payload, sizeof(payload), 1200u), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 0);

    /*
     * A PREPARED write that succeeds but whose verification read fails is
     * recovered as PREPARED and can be retried exactly.
     */
    app_mesh_persistence_test_fail_gateway_click_metadata_verify(-EIO, 1u);
    zassert_equal(app_mesh_persistence_prepare_gateway_host_journal(
                      &packet, payload, sizeof(payload), 1201u), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED);

    app_mesh_persistence_test_fail_gateway_click_metadata_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_commit_gateway_host_journal(
                      &packet, payload, sizeof(payload)), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED);

    /*
     * A COMMITTED marker may be durable even if its verification read fails;
     * exact retry recognizes that terminal phase without reapplying semantics.
     */
    app_mesh_persistence_test_fail_gateway_click_metadata_verify(-EIO, 1u);
    zassert_equal(app_mesh_persistence_commit_gateway_host_journal(
                      &packet, payload, sizeof(payload)), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);
    zassert_ok(app_mesh_persistence_commit_gateway_host_journal(
        &packet, payload, sizeof(payload)));

    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_rejects_non_ack_and_non_host_records)
{
    const uint8_t payload[] = { 0x5au };
    struct proto_packet packet = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 0x2310u, sizeof(payload));

    zassert_true(app_mesh_persistence_gateway_host_journal_supports(&packet));
    packet.flags &= (uint8_t)~FLAG_GATEWAY_ACK_REQUIRED;
    zassert_false(app_mesh_persistence_gateway_host_journal_supports(&packet));
    zassert_equal(app_mesh_persistence_save_gateway_host_journal(
                      &packet, payload, sizeof(payload), 800u), -EINVAL);

    packet = make_gateway_host_packet(MSG_MESH_DATA,
                                      0x2311u,
                                      sizeof(payload));
    zassert_false(app_mesh_persistence_gateway_host_journal_supports(&packet));
    zassert_equal(app_mesh_persistence_save_gateway_host_journal(
                      &packet, payload, sizeof(payload), 801u), -EINVAL);
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_serializes_different_record_classes)
{
    const uint8_t first_payload[] = { 0x31u, 0x32u };
    const uint8_t second_payload[] = { 0x41u, 0x42u, 0x43u };
    struct proto_packet first = make_gateway_host_packet(
        MSG_COMMAND_RESULT, 0x2320u, sizeof(first_payload));
    struct proto_packet second = make_gateway_host_packet(
        MSG_SURVEY_PAIR_RESULT, 0x2321u, sizeof(second_payload));

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &first, first_payload, sizeof(first_payload), 900u));
    zassert_equal(app_mesh_persistence_gateway_host_journal_matches(
                      &second,
                      second_payload,
                      sizeof(second_payload)), -EBUSY);
    zassert_equal(app_mesh_persistence_save_gateway_host_journal(
                      &second,
                      second_payload,
                      sizeof(second_payload),
                      901u), -EBUSY);
    zassert_equal(app_mesh_persistence_gateway_host_journal_matches(
                      &first,
                      first_payload,
                      sizeof(first_payload)), 1);
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &first, first_payload, sizeof(first_payload)));
}

ZTEST(mesh_persistence, test_gateway_click_journal_idempotence_and_conflict)
{
    const uint8_t payload[] = { 0x10u, 0x20u, 0x30u };
    const uint8_t other_payload[] = { 0x40u, 0x50u, 0x60u };
    struct proto_packet packet = make_gateway_click_packet(
        0x2202u, sizeof(payload));
    struct proto_packet other = packet;

    other.seq++;
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 100u));
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 101u));
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &packet, payload, sizeof(payload)), 1);
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &other, other_payload, sizeof(other_payload)), -EBUSY);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &other, other_payload, sizeof(other_payload), 102u), -EBUSY);
}

ZTEST(mesh_persistence,
      test_gateway_host_journal_rejects_crc16_payload_collision)
{
    static uint8_t first[128];
    static uint8_t second[128];
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 1510u,
        .node_id = CHILD_ID,
        .node_boot_counter = 61u,
        .result_seq = 11u,
    };
    struct proto_packet packet;
    struct proto_packet restored_packet;
    uint8_t restored[128];
    uint8_t first_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t second_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t first_len;
    size_t second_len;
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    first_len = build_collection_command_result(first,
                                                sizeof(first),
                                                &result_id,
                                                2510u,
                                                0u);
    zassert_equal(tlv_append_u16(first,
                                 sizeof(first),
                                 &first_len,
                                 TLV_FW_VERSION,
                                 UINT16_C(0x3037)),
                  PROTO_OK);
    second_len = build_collection_command_result(second,
                                                 sizeof(second),
                                                 &result_id,
                                                 2510u,
                                                 1u);
    zassert_equal(tlv_append_u16(second,
                                 sizeof(second),
                                 &second_len,
                                 TLV_FW_VERSION,
                                 0u),
                  PROTO_OK);
    zassert_equal(first_len, second_len);
    zassert_not_equal(memcmp(first, second, first_len), 0);
    zassert_equal(proto_crc16_ccitt_false(first, first_len),
                  proto_crc16_ccitt_false(second, second_len));
    test_payload_digest(first, first_len, first_digest);
    test_payload_digest(second, second_len, second_digest);
    zassert_not_equal(memcmp(first_digest,
                             second_digest,
                             sizeof(first_digest)),
                      0);
    packet = make_gateway_host_packet(MSG_COMMAND_RESULT,
                                      0x2322u,
                                      (uint16_t)first_len);

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal());
    zassert_ok(app_mesh_persistence_save_gateway_host_journal(
        &packet, first, first_len, 905u));
    zassert_equal(app_mesh_persistence_gateway_host_journal_matches(
                      &packet, second, second_len),
                  -EBUSY);
    zassert_equal(app_mesh_persistence_save_gateway_host_journal(
                      &packet, second, second_len, 906u),
                  -EBUSY);
    zassert_equal(app_mesh_persistence_restore_gateway_host_journal(
                      &restored_packet,
                      restored,
                      sizeof(restored),
                      &restored_len,
                      &restored_at_ms),
                  APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);
    zassert_equal(restored_len, first_len);
    zassert_mem_equal(restored, first, first_len);
    zassert_equal(restored_at_ms, 905u);
    zassert_equal(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, second_digest),
        -ESTALE);
    zassert_equal(app_mesh_persistence_clear_gateway_host_journal_if_matches(
                      &packet, second, second_len),
                  -ESTALE);
    zassert_ok(
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, first_digest));
    zassert_ok(app_mesh_persistence_clear_gateway_host_journal_if_matches(
        &packet, first, first_len));
}

ZTEST(mesh_persistence, test_gateway_click_journal_retry_ignores_ttl_and_age)
{
    const uint8_t payload[] = { 0x71u, 0x82u, 0x93u, 0xa4u };
    uint8_t changed_payload[sizeof(payload)];
    struct proto_packet packet = make_gateway_click_packet(
        0x2205u, sizeof(payload));
    struct proto_packet restored_packet;
    struct proto_packet flag_mutation;
    uint8_t restored_payload[PACKET_MAX_PAYLOAD_LEN];
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    memcpy(changed_payload, payload, sizeof(changed_payload));
    changed_payload[0] ^= 0x01u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 500u));

    /* Restore through NVS to model a reboot, then receive the same report
     * after another relay rewrote its transport-local header fields. */
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    restored_packet.ttl = 1u;
    restored_packet.message_age_ms = 0xfeedbeefu;
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &restored_packet, payload, sizeof(payload)), 1);
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &restored_packet, payload, sizeof(payload), 501u));

    /* A payload mutation and a semantic-flag mutation remain conflicts even
     * when the transport fields otherwise look like a retry. */
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &restored_packet,
                      changed_payload,
                      sizeof(changed_payload)), -EBUSY);
    flag_mutation = restored_packet;
    flag_mutation.flags ^= FLAG_COUNT_AS_CLICK;
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &flag_mutation, payload, sizeof(payload)), -EBUSY);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &flag_mutation,
                      payload,
                      sizeof(payload),
                      502u), -EBUSY);

    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
}

ZTEST(mesh_persistence, test_gateway_click_journal_faults_retain_custody)
{
    const uint8_t payload[] = { 0x61u, 0x62u, 0x63u, 0x64u };
    struct proto_packet packet = make_gateway_click_packet(
        0x2203u, sizeof(payload));
    struct proto_packet replacement = packet;
    struct proto_packet restored_packet;
    uint8_t restored_payload[PACKET_MAX_PAYLOAD_LEN];
    size_t restored_len;
    uint32_t restored_at_ms;

    replacement.seq++;
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    app_mesh_persistence_test_fail_gateway_click_payload_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &packet, payload, sizeof(payload), 200u), -EIO);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 0);

    app_mesh_persistence_test_reset_faults();
    app_mesh_persistence_test_fail_gateway_click_metadata_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &packet, payload, sizeof(payload), 201u), -EIO);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 0);

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 202u));
    app_mesh_persistence_test_fail_gateway_click_metadata_read(-EIO, 1u);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);

    app_mesh_persistence_test_fail_gateway_click_payload_read(-EIO, 1u);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);

    /*
     * A malformed metadata read may describe the only record left after the
     * gateway ACK.  Restore, duplicate classification, and replacement
     * admission must all retain it and fail closed until the fault is repaired.
     */
    app_mesh_persistence_test_fail_gateway_click_metadata_read(
        -EBADMSG, 3u);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EBADMSG);
    zassert_equal(app_mesh_persistence_gateway_click_journal_matches(
                      &packet, payload, sizeof(payload)), -EBADMSG);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &replacement,
                      payload,
                      sizeof(payload),
                      203u), -EBADMSG);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);

    app_mesh_persistence_test_fail_gateway_click_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_gateway_click_journal(), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
}

ZTEST(mesh_persistence, test_gateway_click_journal_extended_payload_and_torn_clear)
{
    static uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    static uint8_t restored_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    static uint8_t malformed_payload[1];
    struct proto_packet packet = make_gateway_click_packet(
        0x2204u, sizeof(payload));
    struct proto_packet replacement = packet;
    struct proto_packet restored_packet;
    size_t restored_len = 0u;
    uint32_t restored_at_ms = 0u;

    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 37u + 11u);
    }
    replacement.seq++;
    malformed_payload[0] = 0xa5u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 400u));
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    zassert_equal(restored_len, sizeof(payload));
    zassert_mem_equal(restored_payload, payload, sizeof(payload));
    zassert_equal(restored_at_ms, 400u);
    zassert_equal(restored_packet.seq, packet.seq);
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    /* A failed 958-byte write leaves no commit marker and is retryable. */
    app_mesh_persistence_test_fail_gateway_click_payload_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &packet, payload, sizeof(payload), 401u), -EIO);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 0);
    app_mesh_persistence_test_reset_faults();

    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 402u));
    app_mesh_persistence_test_fail_gateway_click_payload_read(-EIO, 1u);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EIO);
    app_mesh_persistence_test_reset_faults();
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    /*
     * Missing and truncated payload records are permanent corruption.  The
     * metadata marker remains sole-custody evidence and blocks replacement
     * admission until an explicit repair clears it.
     */
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 403u));
    zassert_ok(app_mesh_persistence_test_delete_gateway_click_payload());
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EBADMSG);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EBADMSG);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &replacement,
                      payload,
                      sizeof(payload),
                      404u), -EBUSY);
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 405u));
    zassert_ok(app_mesh_persistence_test_write_gateway_click_payload(
        malformed_payload, sizeof(malformed_payload)));
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EBADMSG);
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), -EBADMSG);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &replacement,
                      payload,
                      sizeof(payload),
                      406u), -EBUSY);
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    /* A payload-delete failure leaves the marker visible, so a newer click
     * cannot overwrite it while the clear retry is pending. */
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 407u));
    app_mesh_persistence_test_fail_gateway_click_payload_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_gateway_click_journal(), -EIO);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &replacement, payload, sizeof(payload), 408u), -EBUSY);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());

    /* A metadata-delete failure after payload deletion leaves the marker in
     * place, so a newer click cannot overwrite it; retry clears the old
     * identity before replacement admission. */
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &packet, payload, sizeof(payload), 409u));
    app_mesh_persistence_test_fail_gateway_click_metadata_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_gateway_click_journal(), -EIO);
    zassert_equal(app_mesh_persistence_save_gateway_click_journal(
                      &replacement, payload, sizeof(payload), 410u), -EBUSY);
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
    zassert_ok(app_mesh_persistence_save_gateway_click_journal(
        &replacement, payload, sizeof(payload), 411u));
    zassert_equal(app_mesh_persistence_restore_gateway_click_journal(
                      &restored_packet,
                      restored_payload,
                      sizeof(restored_payload),
                      &restored_len,
                      &restored_at_ms), 1);
    zassert_equal(restored_packet.seq, replacement.seq);
    zassert_equal(restored_len, sizeof(payload));
    zassert_mem_equal(restored_payload, payload, sizeof(payload));
    zassert_ok(app_mesh_persistence_clear_gateway_click_journal());
}

static void assert_membership_roster_equal(
    const struct gateway_membership_roster *actual,
    const struct gateway_membership_roster *expected)
{
    zassert_equal(actual->valid, expected->valid);
    zassert_equal(actual->membership_epoch, expected->membership_epoch);
    zassert_equal(actual->node_count, expected->node_count);
    zassert_equal(actual->slot_span, expected->slot_span);
    zassert_mem_equal(actual->node_ids,
                      expected->node_ids,
                      sizeof(expected->node_ids));
}

static void build_identity_command_result_payload(
    uint8_t *payload,
    size_t payload_cap,
    size_t target_len,
    const struct command_result_id *result_id,
    size_t *payload_len)
{
    uint8_t padding[24];

    zassert_not_null(payload);
    zassert_not_null(result_id);
    zassert_not_null(payload_len);
    zassert_true(target_len <= payload_cap);

    *payload_len = 0u;
    zassert_ok(command_result_id_append_tlvs(payload,
                                             payload_cap,
                                             payload_len,
                                             result_id));
    zassert_ok(mesh_append_command_result(payload,
                                          payload_cap,
                                          payload_len,
                                          CMD_GET_STATUS,
                                          COMMAND_OK,
                                          0u));
    memset(padding, 0xA5, sizeof(padding));
    while (*payload_len < target_len) {
        const size_t remaining = target_len - *payload_len;
        const uint8_t chunk_len = remaining > sizeof(padding) + 2u ?
                                  (uint8_t)sizeof(padding) :
                                  (uint8_t)(remaining > 2u ? remaining - 2u : 0u);

        zassert_true(chunk_len > 0u);
        zassert_ok(tlv_append_bytes(payload,
                                    payload_cap,
                                    payload_len,
                                    TLV_MESH_TEST_PADDING,
                                    padding,
                                    chunk_len));
    }
    zassert_equal(*payload_len, target_len);
}

static void build_collection_command_result_payload(
    uint8_t *payload,
    size_t payload_cap,
    size_t target_len,
    const struct command_result_id *result_id,
    uint32_t collection_epoch_id,
    size_t *payload_len)
{
    build_identity_command_result_payload(payload,
                                          payload_cap,
                                          target_len,
                                          result_id,
                                          payload_len);
    zassert_ok(tlv_append_u32(payload,
                              payload_cap,
                              payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              collection_epoch_id));
}

static void init_deferred_outbox_relay(struct mesh_relay *relay,
                                       uint32_t command_seq,
                                       uint32_t start_ms)
{
    struct route_candidate route = direct_gateway_route(90u);
    struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = command_seq,
        .node_id = LOCAL_ID,
        .node_boot_counter = command_seq + 100u,
        .result_seq = (uint16_t)(command_seq + 101u),
    };
    struct proto_packet packet = {0};
    struct mesh_outbound tx;
    uint8_t payload[128];
    size_t payload_len = 0u;

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            command_seq + 2000u,
                                            &payload_len);
    zassert_ok(mesh_init_command_result(&packet,
                                        LOCAL_ID,
                                        GATEWAY_ID,
                                        command_seq,
                                        result_id.result_seq,
                                        (uint8_t)payload_len,
                                        false));
    mesh_relay_init(relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&relay->upstream, &route));
    zassert_ok(mesh_relay_start_tx(relay,
                                   &packet,
                                   payload,
                                   payload_len,
                                   start_ms,
                                   &tx));
}

static struct app_mesh_collection_result_snapshot make_snapshot(void)
{
    return (struct app_mesh_collection_result_snapshot) {
        .version = APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .command = {
            .msg_type = MSG_COMMAND,
            .src_id = GATEWAY_ID,
            .dst_id = LOCAL_ID,
            .session_id = 1001u,
            .seq = 7u,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 3u,
        },
        .result_id = {
            .gateway_id = GATEWAY_ID,
            .gateway_epoch = 13u,
            .command_seq = 1001u,
            .node_id = LOCAL_ID,
            .node_boot_counter = 55u,
            .result_seq = 56u,
        },
        .collection_epoch_id = 3003u,
        .delay_ms = 12345u,
        .command_id = CMD_GET_STATUS,
        .status = COMMAND_OK,
        .reason = 9u,
        .force_rediscovery_after_result = true,
        .reboot_after_result = false,
        .valid = true,
    };
}

static struct gateway_membership_roster make_gateway_membership_roster(void)
{
    const uint64_t node_ids[] = {
        MEMBER_A_ID,
        MEMBER_B_ID,
        MEMBER_C_ID,
    };
    struct gateway_membership_roster roster;

    gateway_membership_clear(&roster);
    zassert_ok(gateway_membership_set_roster_preserve_order(&roster,
                                                            44u,
                                                            node_ids,
                                                            sizeof(node_ids) / sizeof(node_ids[0])));
    return roster;
}

static struct gateway_membership_roster
make_sparse_gateway_membership_roster(void)
{
    const uint64_t node_ids[] = {
        MEMBER_A_ID,
        MEMBER_B_ID,
        MEMBER_C_ID,
    };
    const uint8_t slots[] = {0u, 1u, 3u};
    struct gateway_membership_roster roster;

    gateway_membership_clear(&roster);
    zassert_ok(gateway_membership_set_roster_explicit_slots(
        &roster,
        45u,
        node_ids,
        slots,
        ARRAY_SIZE(node_ids)));
    return roster;
}

static struct gateway_membership_publication
make_pending_gateway_membership_publication(void)
{
    struct gateway_membership_publication publication = {0};

    publication.claimed_node_ids[0] = MEMBER_A_ID;
    publication.claimed_node_ids[1] = MEMBER_B_ID;
    publication.claimed_node_ids[3] = MEMBER_C_ID;
    publication.claimed_node_ids[4] = MEMBER_D_ID;
    publication.host_command = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = UINT64_C(0x1010),
        .dst_id = MESH_BROADCAST_ID,
        .session_id = UINT32_C(0x1234abcd),
        .seq = UINT16_C(0x4567),
        .ttl = 0u,
        .payload_len = PROTO_TLV_U16_ENCODED_LEN,
        .message_age_ms = 11u,
    };
    publication.committed_mask =
        (UINT64_C(1) << 0) |
        (UINT64_C(1) << 1) |
        (UINT64_C(1) << 3);
    publication.acknowledged_mask =
        (UINT64_C(1) << 0) |
        (UINT64_C(1) << 3);
    publication.command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication.event_gateway_epoch = UINT16_C(0x2345);
    publication.duplicate_count = 7u;
    publication.claimed_count = 4u;
    publication.claimed_slot_span = 5u;
    publication.table_round = 2u;
    publication.publish_pending = 1u;
    return publication;
}

static void assert_gateway_membership_publication_equal(
    const struct gateway_membership_publication *actual,
    const struct gateway_membership_publication *expected)
{
    zassert_mem_equal(actual->claimed_node_ids,
                      expected->claimed_node_ids,
                      sizeof(expected->claimed_node_ids));
    zassert_equal(actual->host_command.msg_type,
                  expected->host_command.msg_type);
    zassert_equal(actual->host_command.flags, expected->host_command.flags);
    zassert_equal(actual->host_command.src_id, expected->host_command.src_id);
    zassert_equal(actual->host_command.dst_id, expected->host_command.dst_id);
    zassert_equal(actual->host_command.session_id,
                  expected->host_command.session_id);
    zassert_equal(actual->host_command.seq, expected->host_command.seq);
    zassert_equal(actual->host_command.ttl, expected->host_command.ttl);
    zassert_equal(actual->host_command.payload_len,
                  expected->host_command.payload_len);
    zassert_equal(actual->host_command.message_age_ms,
                  expected->host_command.message_age_ms);
    zassert_equal(actual->committed_mask, expected->committed_mask);
    zassert_equal(actual->acknowledged_mask, expected->acknowledged_mask);
    zassert_equal(actual->command_id, expected->command_id);
    zassert_equal(actual->event_gateway_epoch,
                  expected->event_gateway_epoch);
    zassert_equal(actual->duplicate_count, expected->duplicate_count);
    zassert_equal(actual->claimed_count, expected->claimed_count);
    zassert_equal(actual->claimed_slot_span, expected->claimed_slot_span);
    zassert_equal(actual->table_round, expected->table_round);
    zassert_equal(actual->publish_pending, expected->publish_pending);
}

ZTEST(mesh_persistence, test_collection_result_snapshot_round_trip_and_clear)
{
    struct app_mesh_collection_result_snapshot saved = make_snapshot();
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_collection_result());

    zassert_ok(app_mesh_persistence_save_collection_result(&saved));
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    zassert_ok(app_mesh_persistence_clear_collection_result());
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence,
      test_collection_result_clear_failure_retains_exact_custody)
{
    struct app_mesh_collection_result_snapshot saved = make_snapshot();
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_save_collection_result(&saved));

    app_mesh_persistence_test_fail_collection_result_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_collection_result(), -EIO);
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_clear_collection_result());
}

ZTEST(mesh_persistence,
      test_collection_result_legacy_snapshot_migrates_before_restore)
{
    struct app_mesh_collection_result_snapshot saved = make_snapshot();
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_test_write_collection_result_raw(
        &saved, sizeof(saved)));

    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    /* The second restore reads the checksummed wrapper written by migration. */
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));
    zassert_ok(app_mesh_persistence_clear_collection_result());
}

ZTEST(mesh_persistence,
      test_collection_result_corruption_remains_fail_closed)
{
    const uint8_t corrupt[] = {0xA5u, 0x5Au, 0x11u};
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_test_write_collection_result_raw(
        corrupt, sizeof(corrupt)));

    memset(&restored, 0, sizeof(restored));
    zassert_equal(
        app_mesh_persistence_restore_collection_result(&restored), -EINVAL);
    memset(&restored, 0, sizeof(restored));
    zassert_equal(
        app_mesh_persistence_restore_collection_result(&restored), -EINVAL);

    zassert_ok(app_mesh_persistence_clear_collection_result());
}

ZTEST(mesh_persistence, test_collection_result_snapshot_rejects_invalid_save)
{
    struct app_mesh_collection_result_snapshot snapshot = make_snapshot();

    zassert_ok(app_mesh_persistence_init());

    snapshot.valid = false;
    zassert_equal(app_mesh_persistence_save_collection_result(&snapshot), -EINVAL);

    snapshot = make_snapshot();
    snapshot.version++;
    zassert_equal(app_mesh_persistence_save_collection_result(&snapshot), -EINVAL);

    zassert_equal(app_mesh_persistence_save_collection_result(NULL), -EINVAL);
}

ZTEST(mesh_persistence, test_gateway_collection_snapshot_round_trip_and_clear)
{
    const uint64_t expected_roster[] = {
        LOCAL_ID,
        CHILD_ID,
        MEMBER_A_ID,
    };
    const struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 0x1201u,
        .node_id = LOCAL_ID,
        .node_boot_counter = 101u,
        .result_seq = 102u,
    };
    struct command_result_id id_b = id_a;
    struct command_result_id unknown_id = id_a;
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    struct proto_packet packet_a = {0};
    struct proto_packet packet_b = {0};
    uint64_t candidates[2] = {0};
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    bool duplicate = false;

    id_b.node_id = CHILD_ID;
    id_b.node_boot_counter = 201u;
    id_b.result_seq = 202u;
    unknown_id.node_id = MEMBER_B_ID;
    unknown_id.node_boot_counter = 301u;
    unknown_id.result_seq = 302u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
    zassert_ok(app_mesh_persistence_clear_gateway_collection());

    zassert_ok(gateway_collection_start(&collection,
                                        GATEWAY_ID,
                                        13u,
                                        id_a.command_seq,
                                        3013u,
                                        4u,
                                        3u,
                                        1u,
                                        1200u));
    zassert_ok(gateway_collection_set_expected_roster(
        &collection,
        expected_roster,
        ARRAY_SIZE(expected_roster)));
    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &id_a,
                                            collection.collection_epoch_id,
                                            &payload_a_len);
    build_collection_command_result_payload(payload_b,
                                            sizeof(payload_b),
                                            64u,
                                            &id_b,
                                            collection.collection_epoch_id,
                                            &payload_b_len);
    zassert_ok(mesh_init_command_result(&packet_a,
                                        id_a.node_id,
                                        GATEWAY_ID,
                                        id_a.command_seq,
                                        id_a.result_seq,
                                        (uint8_t)payload_a_len,
                                        false));
    zassert_ok(mesh_init_command_result(&packet_b,
                                        id_b.node_id,
                                        GATEWAY_ID,
                                        id_b.command_seq,
                                        id_b.result_seq,
                                        (uint8_t)payload_b_len,
                                        false));
    zassert_ok(gateway_collection_record_result_from_hop(&collection,
                                                         &packet_a,
                                                         payload_a,
                                                         payload_a_len,
                                                         0xAAAABBBBCCCC0001ull,
                                                         &duplicate));
    zassert_false(duplicate);
    zassert_ok(gateway_collection_record_result_from_hop(&collection,
                                                         &packet_b,
                                                         payload_b,
                                                         payload_b_len,
                                                         0xAAAABBBBCCCC0002ull,
                                                         &duplicate));
    zassert_false(duplicate);

    zassert_ok(app_mesh_persistence_save_gateway_collection(&collection));

    gateway_collection_clear(&restored);
    zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
    zassert_equal(restored.gateway_id, collection.gateway_id);
    zassert_equal(restored.gateway_epoch, collection.gateway_epoch);
    zassert_equal(restored.command_seq, collection.command_seq);
    zassert_equal(restored.collection_epoch_id, collection.collection_epoch_id);
    zassert_equal(restored.membership_epoch, collection.membership_epoch);
    zassert_equal(restored.expected_count, collection.expected_count);
    zassert_equal(restored.received_count, 2u);
    zassert_equal(restored.retry_round, 1u);
    zassert_equal(restored.next_retry_spread_ms, 1200u);
    zassert_true(restored.collection_open);
    zassert_true(restored.eack_pending);
    zassert_equal(restored.expected_node_id_count, ARRAY_SIZE(expected_roster));
    zassert_mem_equal(restored.expected_node_ids,
                      expected_roster,
                      sizeof(expected_roster));
    assert_collection_result_id_equal(&restored, &restored.results[0].id, &id_a);
    assert_collection_result_id_equal(&restored, &restored.results[1].id, &id_b);
    zassert_equal(restored.results[0].previous_hop_id, 0xAAAABBBBCCCC0001ull);
    zassert_equal(restored.results[1].previous_hop_id, 0xAAAABBBBCCCC0002ull);
    zassert_equal(gateway_collection_return_candidates(&restored,
                                                       candidates,
                                                       sizeof(candidates) / sizeof(candidates[0])),
                  2u);
    zassert_equal(candidates[0], 0xAAAABBBBCCCC0002ull);
    zassert_equal(candidates[1], 0xAAAABBBBCCCC0001ull);

    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &unknown_id,
                                            restored.collection_epoch_id,
                                            &payload_a_len);
    zassert_ok(mesh_init_command_result(&packet_a,
                                        unknown_id.node_id,
                                        GATEWAY_ID,
                                        unknown_id.command_seq,
                                        unknown_id.result_seq,
                                        (uint8_t)payload_a_len,
                                        false));
    duplicate = true;
    zassert_equal(gateway_collection_record_result_from_hop(
                      &restored,
                      &packet_a,
                      payload_a,
                      payload_a_len,
                      unknown_id.node_id,
                      &duplicate),
                  PROTO_ERR_NOT_FOUND);
    zassert_equal(restored.received_count, 2u);

    zassert_ok(app_mesh_persistence_clear_gateway_collection());
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
    zassert_equal(restored.gateway_id, 0u);
    zassert_false(restored.collection_open);
}

ZTEST(mesh_persistence, test_gateway_collection_journal_50_node_nvs_gc_and_reuse)
{
    static struct gateway_collection_state collection;
    static struct gateway_collection_state restored;
    static uint64_t roster[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];
    uint8_t payload[96];

    zassert_equal(ARRAY_SIZE(roster), GATEWAY_COLLECTION_RESULT_CACHE_SIZE);
    for (size_t slot = 0u; slot < ARRAY_SIZE(roster); slot++) {
        roster[slot] = UINT64_C(0xA500000000000000) + slot + 1u;
    }

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
    zassert_ok(app_mesh_persistence_clear_gateway_collection());

    for (uint8_t generation = 0u; generation < 2u; generation++) {
        uint32_t command_seq = 0x5000u + generation;
        uint32_t collection_epoch_id = 0x7000u + generation;

        zassert_ok(gateway_collection_start(&collection,
                                            GATEWAY_ID,
                                            13u,
                                            command_seq,
                                            collection_epoch_id,
                                            51u,
                                            ARRAY_SIZE(roster),
                                            0u,
                                            1000u));
        zassert_ok(gateway_collection_set_expected_roster(&collection,
                                                           roster,
                                                           ARRAY_SIZE(roster)));
        zassert_ok(app_mesh_persistence_save_gateway_collection(&collection));

        for (size_t slot = 0u; slot < ARRAY_SIZE(roster); slot++) {
            struct command_result_id result_id = {
                .gateway_id = GATEWAY_ID,
                .gateway_epoch = 13u,
                .command_seq = command_seq,
                .node_id = roster[slot],
                .node_boot_counter = 0x10000u +
                                     ((uint32_t)generation * 100u) +
                                     (uint32_t)slot,
                .result_seq = (uint16_t)(((uint16_t)generation * 100u) +
                                         (uint16_t)slot + 1u),
            };
            struct proto_packet packet = {0};
            size_t payload_len = 0u;
            bool duplicate = false;

            build_collection_command_result_payload(payload,
                                                    sizeof(payload),
                                                    64u,
                                                    &result_id,
                                                    collection_epoch_id,
                                                    &payload_len);
            zassert_ok(mesh_init_command_result(&packet,
                                                result_id.node_id,
                                                GATEWAY_ID,
                                                command_seq,
                                                result_id.result_seq,
                                                (uint8_t)payload_len,
                                                false));
            zassert_ok(gateway_collection_record_result_from_hop(
                &collection,
                &packet,
                payload,
                payload_len,
                result_id.node_id,
                &duplicate));
            zassert_false(duplicate);
            zassert_ok(app_mesh_persistence_save_gateway_collection(&collection));
        }

        gateway_collection_clear(&restored);
        zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
        zassert_equal(restored.command_seq, command_seq);
        zassert_equal(restored.collection_epoch_id, collection_epoch_id);
        zassert_equal(restored.received_count, ARRAY_SIZE(roster));
        zassert_false(restored.collection_open);
        zassert_true(restored.eack_pending);
        zassert_mem_equal(restored.expected_node_ids,
                          roster,
                          sizeof(roster));
        for (size_t slot = 0u; slot < ARRAY_SIZE(roster); slot++) {
            zassert_true(restored.results[slot].valid);
            zassert_equal(restored.results[slot].id.node_id, roster[slot]);
            zassert_equal(restored.results[slot].previous_hop_id, roster[slot]);
        }

        zassert_ok(app_mesh_persistence_clear_gateway_collection());
        memset(&restored, 0xA5, sizeof(restored));
        zassert_ok(app_mesh_persistence_restore_gateway_collection(&restored));
        zassert_equal(restored.gateway_id, 0u);
    }
}

ZTEST(mesh_persistence, test_gateway_collection_snapshot_rejects_invalid_save)
{
    struct gateway_collection_state collection;

    zassert_ok(app_mesh_persistence_init());

    gateway_collection_clear(&collection);
    zassert_equal(app_mesh_persistence_save_gateway_collection(&collection), -EINVAL);
    zassert_equal(app_mesh_persistence_save_gateway_collection(NULL), -EINVAL);

    zassert_ok(gateway_collection_start(&collection,
                                        GATEWAY_ID,
                                        13u,
                                        0x1211u,
                                        3021u,
                                        4u,
                                        1u,
                                        0u,
                                        COLLECTION_RETRY_ROUND_0_MS));
    collection.received_count = 1u;
    zassert_equal(app_mesh_persistence_save_gateway_collection(&collection), -EINVAL);

    zassert_ok(gateway_collection_start(&collection,
                                        GATEWAY_ID,
                                        13u,
                                        0x1211u,
                                        3021u,
                                        4u,
                                        1u,
                                        0u,
                                        COLLECTION_RETRY_ROUND_0_MS));
    collection.eack_pending = false;
    zassert_equal(app_mesh_persistence_save_gateway_collection(&collection), -EINVAL);
}

ZTEST(mesh_persistence, test_gateway_eack_custody_round_trip_and_clear)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 0u,
        .packet_sequence = 700u,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 5u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_STEADY_MS,
        .collection_open = true,
    };
    struct gateway_collection_eack_custody_snapshot saved = {0};
    struct gateway_collection_eack_custody_snapshot restored = {0};
    struct app_mesh_persistence_health health = {0};
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 700u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_eack_custody());
    zassert_ok(gateway_collection_eack_append_tlvs(payload,
                                                   sizeof(payload),
                                                   &payload_len,
                                                   &eack));
    packet.payload_len = (uint16_t)payload_len;
    zassert_ok(gateway_collection_eack_custody_capture(&saved,
                                                       &packet,
                                                       payload,
                                                       payload_len));
    zassert_ok(app_mesh_persistence_save_gateway_eack_custody(&saved));
    zassert_ok(app_mesh_persistence_restore_gateway_eack_custody(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    app_mesh_persistence_test_fail_gateway_eack_custody_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_gateway_eack_custody(), -EIO);
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.last_error, -EIO);
    zassert_true(health.consecutive_failures > 0u);

    /*
     * A failed tombstone keeps the exact snapshot observable. Runtime must
     * not freeze a replacement EACK until the retry proves this key absent.
     */
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_eack_custody(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    zassert_ok(app_mesh_persistence_clear_gateway_eack_custody());
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_eack_custody(&restored));
    zassert_false(restored.valid);
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.consecutive_failures, 0u);
    zassert_equal(health.last_error, 0);
}

ZTEST(mesh_persistence, test_gateway_eack_custody_rejects_corruption)
{
    struct gateway_collection_eack_custody_snapshot snapshot = {0};
    struct gateway_collection_eack_custody_snapshot restored = {0};
    struct app_mesh_persistence_health health = {0};
    const uint8_t truncated[] = {0xA5u, 0x5Au, 0x11u};

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_eack_custody());
    snapshot.version = GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_VERSION;
    snapshot.valid = true;
    snapshot.payload_len = 1u;
    snapshot.payload[0] = 0xA5u;
    snapshot.payload_crc = proto_crc16_ccitt_false(snapshot.payload,
                                                   snapshot.payload_len);
    zassert_equal(app_mesh_persistence_save_gateway_eack_custody(&snapshot),
                  -EINVAL);
    zassert_equal(app_mesh_persistence_save_gateway_eack_custody(NULL),
                  -EINVAL);

    zassert_ok(app_mesh_persistence_test_write_gateway_eack_custody_raw(
        truncated, sizeof(truncated)));
    zassert_equal(app_mesh_persistence_restore_gateway_eack_custody(&restored),
                  -EBADMSG);
    app_mesh_persistence_get_health(&health);
    zassert_true(health.consecutive_failures > 0u);
    zassert_equal(health.last_error, -EBADMSG);

    /*
     * A second restore must observe the same corruption.  If restore silently
     * deleted the key, this would return success and make lost custody look
     * like a clean boot.
     */
    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(app_mesh_persistence_restore_gateway_eack_custody(&restored),
                  -EBADMSG);
    zassert_ok(app_mesh_persistence_clear_gateway_eack_custody());
    zassert_ok(app_mesh_persistence_restore_gateway_eack_custody(&restored));
    zassert_false(restored.valid);
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.consecutive_failures, 0u);
    zassert_equal(health.last_error, 0);
}

ZTEST(mesh_persistence, test_gateway_membership_snapshot_round_trip_and_clear)
{
    struct gateway_membership_roster saved = make_gateway_membership_roster();
    struct gateway_membership_roster restored;
    bool publication_pending = true;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());

    zassert_ok(app_mesh_persistence_save_gateway_membership(&saved));

    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored, &publication_pending));
    zassert_false(publication_pending);
    assert_membership_roster_equal(&restored, &saved);

    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    memset(&restored, 0xA5, sizeof(restored));
    publication_pending = true;
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored, &publication_pending));
    zassert_false(publication_pending);
    zassert_false(restored.valid);
    zassert_equal(restored.membership_epoch, 0u);
    zassert_equal(restored.node_count, 0u);
}

ZTEST(mesh_persistence, test_gateway_membership_snapshot_rejects_invalid_save)
{
    struct gateway_membership_roster roster;

    zassert_ok(app_mesh_persistence_init());

    gateway_membership_clear(&roster);
    zassert_equal(app_mesh_persistence_save_gateway_membership(&roster), -EINVAL);
    zassert_equal(app_mesh_persistence_save_gateway_membership(NULL), -EINVAL);
}

ZTEST(mesh_persistence,
      test_gateway_membership_restore_rejects_without_deleting_bad_nvs)
{
    struct gateway_membership_roster roster = make_gateway_membership_roster();
    struct gateway_membership_roster restored;
    struct gateway_membership_snapshot snapshot;
    uint32_t assignment_epoch = UINT32_MAX;
    bool publication_pending = true;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());

    zassert_ok(gateway_membership_export_snapshot(&roster, &snapshot));
    snapshot.version++;
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &snapshot,
        sizeof(snapshot)));

    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(app_mesh_persistence_restore_gateway_membership(
                      &restored, &publication_pending),
                  -EINVAL);
    zassert_false(publication_pending);
    zassert_false(restored.valid);
    publication_pending = true;
    zassert_equal(app_mesh_persistence_restore_gateway_membership(
                      &restored, &publication_pending),
                  -EINVAL);
    zassert_false(publication_pending);
    zassert_false(restored.valid);
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(
            &assignment_epoch),
        -EINVAL);
    zassert_equal(assignment_epoch, 0u);

    zassert_ok(gateway_membership_export_snapshot(&roster, &snapshot));
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &snapshot,
        sizeof(snapshot) - 1u));

    memset(&restored, 0xA5, sizeof(restored));
    publication_pending = true;
    zassert_equal(app_mesh_persistence_restore_gateway_membership(
                      &restored, &publication_pending),
                  -EINVAL);
    zassert_false(publication_pending);
    zassert_false(restored.valid);
    publication_pending = true;
    zassert_equal(app_mesh_persistence_restore_gateway_membership(
                      &restored, &publication_pending),
                  -EINVAL);
    zassert_false(publication_pending);
    zassert_false(restored.valid);
    assignment_epoch = UINT32_MAX;
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(
            &assignment_epoch),
        -EINVAL);
    zassert_equal(assignment_epoch, 0u);
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
}

ZTEST(mesh_persistence,
      test_gateway_assignment_v4_sparse_pending_publication_survives_reset)
{
    const uint32_t assignment_epoch = UINT32_C(0x80001001);
    const uint32_t table_seq = UINT32_C(0x80001002);
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {
            0x80u, 0x00u, 0x10u, 0x03u, 0x77u, 0x5au, 0x19u, 0xe1u,
            0x11u, 0x29u, 0x40u, 0x58u, 0x61u, 0x7au, 0x83u, 0x9cu,
            0xa5u, 0xbeu, 0xc7u, 0xd0u, 0xe9u, 0xf2u, 0x0bu, 0x14u,
            0x2du, 0x36u, 0x4fu, 0x68u, 0x71u, 0x8au, 0x93u, 0xacu,
        },
    };
    struct gateway_membership_roster roster =
        make_sparse_gateway_membership_roster();
    struct gateway_membership_roster restored_roster;
    struct gateway_membership_publication publication =
        make_pending_gateway_membership_publication();
    struct gateway_membership_publication restored_publication;
    uint32_t restored_assignment_epoch = 0u;
    uint32_t restored_table_seq = 0u;
    struct discovery_assignment_table_commitment restored_table_commitment;
    bool publication_pending = false;

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster,
        assignment_epoch,
        table_seq,
        &table_commitment,
        &publication));

    /* Fresh caller-owned state models the runtime immediately after reset. */
    memset(&restored_roster, 0, sizeof(restored_roster));
    memset(&restored_publication, 0, sizeof(restored_publication));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored_roster, &publication_pending));
    zassert_true(publication_pending);
    assert_membership_roster_equal(&restored_roster, &roster);
    zassert_equal(restored_roster.slot_span, 4u);
    zassert_equal(restored_roster.node_ids[2], 0u);
    zassert_equal(restored_roster.node_ids[3], MEMBER_C_ID);

    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &restored_publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_table_commitment),
        1);
    assert_gateway_membership_publication_equal(&restored_publication,
                                                &publication);
    zassert_equal(restored_assignment_epoch, assignment_epoch);
    zassert_equal(restored_table_seq, table_seq);
    zassert_mem_equal(&restored_table_commitment,
                      &table_commitment,
                      sizeof(table_commitment));
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_A_ID),
                  1);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_B_ID),
                  1);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_C_ID),
                  1);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_D_ID),
                  0);
}

ZTEST(mesh_persistence,
      test_gateway_assignment_publication_completion_rejects_wrong_identity)
{
    const uint32_t assignment_epoch = UINT32_C(0x80002001);
    const uint32_t table_seq = UINT32_C(0x80002002);
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0x80u, 0x00u, 0x20u, 0x03u, 0x31u, 0x75u},
    };
    struct gateway_membership_roster roster =
        make_sparse_gateway_membership_roster();
    struct gateway_membership_publication publication =
        make_pending_gateway_membership_publication();
    struct gateway_membership_publication restored_publication;
    uint32_t restored_assignment_epoch;
    uint32_t restored_table_seq;
    struct discovery_assignment_table_commitment restored_table_commitment;

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster,
        assignment_epoch,
        table_seq,
        &table_commitment,
        &publication));

    zassert_equal(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch + 1u,
            publication.event_gateway_epoch,
            publication.host_command.session_id,
            publication.host_command.seq),
        -ESTALE);
    zassert_equal(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch + 1u,
            publication.host_command.session_id,
            publication.host_command.seq),
        -ESTALE);
    zassert_equal(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch,
            publication.host_command.session_id + 1u,
            publication.host_command.seq),
        -ESTALE);
    zassert_equal(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch,
            publication.host_command.session_id,
            publication.host_command.seq + 1u),
        -ESTALE);

    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &restored_publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_table_commitment),
        1);
    assert_gateway_membership_publication_equal(&restored_publication,
                                                &publication);
    zassert_equal(restored_assignment_epoch, assignment_epoch);
    zassert_equal(restored_table_seq, table_seq);
    zassert_mem_equal(&restored_table_commitment,
                      &table_commitment,
                      sizeof(table_commitment));
}

ZTEST(mesh_persistence,
      test_gateway_assignment_publication_failed_completion_retries_safely)
{
    const uint32_t assignment_epoch = UINT32_C(0x80003001);
    const uint32_t table_seq = UINT32_C(0x80003002);
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0x80u, 0x00u, 0x30u, 0x03u, 0xc8u, 0x44u},
    };
    struct gateway_membership_roster roster =
        make_sparse_gateway_membership_roster();
    struct gateway_membership_roster restored_roster;
    struct gateway_membership_publication publication =
        make_pending_gateway_membership_publication();
    struct gateway_membership_publication restored_publication;
    uint32_t restored_assignment_epoch;
    uint32_t restored_table_seq;
    struct discovery_assignment_table_commitment restored_table_commitment;
    bool publication_pending = false;

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_save_gateway_assignment_membership(
        &roster,
        assignment_epoch,
        table_seq,
        &table_commitment,
        &publication));

    app_mesh_persistence_test_fail_gateway_membership_write(-EIO, 1u);
    zassert_equal(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch,
            publication.host_command.session_id,
            publication.host_command.seq),
        -EIO);
    app_mesh_persistence_test_reset_faults();

    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &restored_publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_table_commitment),
        1);
    assert_gateway_membership_publication_equal(&restored_publication,
                                                &publication);
    zassert_equal(restored_assignment_epoch, assignment_epoch);
    zassert_equal(restored_table_seq, table_seq);
    zassert_mem_equal(&restored_table_commitment,
                      &table_commitment,
                      sizeof(table_commitment));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored_roster, &publication_pending));
    zassert_true(publication_pending);
    assert_membership_roster_equal(&restored_roster, &roster);

    zassert_ok(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch,
            publication.host_command.session_id,
            publication.host_command.seq));

    memset(&restored_publication, 0xA5, sizeof(restored_publication));
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &restored_publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_table_commitment),
        0);
    zassert_false(restored_publication.publish_pending);
    publication_pending = true;
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored_roster, &publication_pending));
    zassert_false(publication_pending);
    assert_membership_roster_equal(&restored_roster, &roster);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_B_ID),
                  1);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_D_ID),
                  0);

    /* Retrying the exact completion after reset is idempotent. */
    zassert_ok(
        app_mesh_persistence_complete_gateway_assignment_publication(
            assignment_epoch,
            publication.event_gateway_epoch,
            publication.host_command.session_id,
            publication.host_command.seq));
}

ZTEST(mesh_persistence,
      test_gateway_membership_v2_migration_drops_legacy_assignment_proof)
{
    const uint32_t assignment_epoch = UINT32_C(0x70004001);
    const uint32_t table_seq = UINT32_C(0x70004002);
    const uint32_t table_fingerprint = UINT32_C(0x70004003);
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0x70u, 0x00u, 0x40u, 0x03u, 0xdeu, 0xadu},
    };
    struct legacy_gateway_membership_snapshot_v2 legacy = {
        .version = 2u,
        .valid = 1u,
        .membership_epoch = 46u,
        .node_count = 3u,
        .node_ids = {MEMBER_A_ID, MEMBER_B_ID, MEMBER_C_ID},
        .assignment_epoch = assignment_epoch,
        .assignment_table_seq = table_seq,
        .assignment_table_fingerprint = table_fingerprint,
        .magic = UINT32_C(0x474D5332),
        .assignment_proof_valid = 1u,
    };
    struct gateway_membership_roster restored_roster;
    struct gateway_membership_publication publication;
    uint32_t restored_assignment_epoch;
    uint32_t restored_table_seq;
    struct discovery_assignment_table_commitment restored_table_commitment;
    uint32_t restored_baseline = 0u;
    bool publication_pending = true;

    zassert_equal(sizeof(legacy), 432u);
    zassert_equal(offsetof(struct legacy_gateway_membership_snapshot_v2,
                           node_ids),
                  8u);
    zassert_equal(offsetof(struct legacy_gateway_membership_snapshot_v2,
                           assignment_epoch),
                  408u);
    zassert_equal(offsetof(struct legacy_gateway_membership_snapshot_v2,
                           checksum),
                  424u);
    legacy.checksum = legacy_gateway_membership_v2_checksum(&legacy);

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &legacy, sizeof(legacy)));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored_roster, &publication_pending));
    zassert_false(publication_pending);
    zassert_true(restored_roster.valid);
    zassert_equal(restored_roster.membership_epoch, 46u);
    zassert_equal(restored_roster.node_count, 3u);
    zassert_equal(restored_roster.slot_span, 3u);
    zassert_equal(restored_roster.node_ids[0], MEMBER_A_ID);
    zassert_equal(restored_roster.node_ids[1], MEMBER_B_ID);
    zassert_equal(restored_roster.node_ids[2], MEMBER_C_ID);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_B_ID),
                  0);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_D_ID),
                  0);
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq + 1u,
                      &table_commitment,
                      MEMBER_B_ID),
                  0);
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(
            &restored_baseline),
        0);
    zassert_equal(restored_baseline, 0u);
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_table_commitment),
        0);
    zassert_equal(restored_assignment_epoch, 0u);
    zassert_equal(restored_table_seq, 0u);
    zassert_mem_equal(
        &restored_table_commitment,
        &(struct discovery_assignment_table_commitment){0},
        sizeof(restored_table_commitment));
}

ZTEST(mesh_persistence,
      test_gateway_membership_v3_migration_drops_proof_and_publication)
{
    const uint32_t assignment_epoch = UINT32_C(0x70005001);
    const uint32_t table_seq = UINT32_C(0x70005002);
    const struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0x70u, 0x00u, 0x50u, 0x03u, 0x5au, 0xa5u},
    };
    struct legacy_gateway_membership_snapshot_v3 legacy = {
        .node_ids = {
            MEMBER_A_ID,
            MEMBER_B_ID,
            0u,
            MEMBER_C_ID,
        },
        .assignment_epoch = assignment_epoch,
        .assignment_table_seq = table_seq,
        .assignment_table_fingerprint = UINT32_C(0x70005003),
        .magic = UINT32_C(0x474D5333),
        .membership_epoch = 47u,
        .version = 3u,
        .node_count = 3u,
        .slot_span = 4u,
        .valid = 1u,
        .assignment_proof_valid = 1u,
    };
    struct gateway_membership_roster restored_roster;
    struct gateway_membership_publication publication;
    struct discovery_assignment_table_commitment restored_commitment;
    uint32_t restored_assignment_epoch = UINT32_MAX;
    uint32_t restored_table_seq = UINT32_MAX;
    uint32_t restored_baseline = UINT32_MAX;
    bool publication_pending = true;

    legacy.publication = make_pending_gateway_membership_publication();
    legacy.checksum = legacy_gateway_membership_v3_checksum(&legacy);
    zassert_equal(sizeof(legacy), 904u);
    zassert_equal(offsetof(struct legacy_gateway_membership_snapshot_v3,
                           assignment_epoch),
                  872u);
    zassert_equal(offsetof(struct legacy_gateway_membership_snapshot_v3,
                           checksum),
                  890u);

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &legacy, sizeof(legacy)));
    zassert_equal(app_mesh_persistence_gateway_assignment_proves(
                      assignment_epoch,
                      table_seq,
                      &table_commitment,
                      MEMBER_A_ID),
                  0);

    zassert_ok(app_mesh_persistence_restore_gateway_membership(
        &restored_roster, &publication_pending));
    zassert_false(publication_pending);
    zassert_true(restored_roster.valid);
    zassert_equal(restored_roster.membership_epoch, 47u);
    zassert_equal(restored_roster.node_count, 3u);
    zassert_equal(restored_roster.slot_span, 4u);
    zassert_equal(restored_roster.node_ids[0], MEMBER_A_ID);
    zassert_equal(restored_roster.node_ids[1], MEMBER_B_ID);
    zassert_equal(restored_roster.node_ids[2], 0u);
    zassert_equal(restored_roster.node_ids[3], MEMBER_C_ID);
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_baseline(
            &restored_baseline),
        0);
    zassert_equal(restored_baseline, 0u);
    zassert_equal(
        app_mesh_persistence_restore_gateway_assignment_publication(
            &publication,
            &restored_assignment_epoch,
            &restored_table_seq,
            &restored_commitment),
        0);
    zassert_equal(restored_assignment_epoch, 0u);
    zassert_equal(restored_table_seq, 0u);
    zassert_mem_equal(
        &restored_commitment,
        &(struct discovery_assignment_table_commitment){0},
        sizeof(restored_commitment));
}

ZTEST(mesh_persistence, test_active_collection_retry_outbox_round_trip)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 0x1009u,
        .node_id = LOCAL_ID,
        .node_boot_counter = 77u,
        .result_seq = 78u,
    };
    struct route_candidate route = direct_gateway_route(90u);
    struct proto_packet packet = {0};
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t timeout_ms;
    uint32_t original_retry_ms;
    uint32_t snapshot_ms;
    uint32_t restore_ms = 250u;
    uint32_t restored_retry_ms;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_outbox();

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3011u,
                                            &result_payload_len);
    zassert_ok(mesh_init_command_result(&packet,
                                        LOCAL_ID,
                                        GATEWAY_ID,
                                        result_id.command_seq,
                                        result_id.result_seq,
                                        (uint8_t)result_payload_len,
                                        false));

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&relay,
                                   &packet,
                                   result_payload,
                                   result_payload_len,
                                   5000u,
                                   &tx));

    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    zassert_ok(mesh_relay_tick(&relay, timeout_ms, &result));
    zassert_true(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    zassert_equal(relay.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_equal(relay.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    zassert_equal(relay.outbox_record.retry_round, 1u);
    original_retry_ms = relay.pending.retry_after_ms;

    snapshot_ms = timeout_ms + 100u;
    zassert_ok(app_mesh_persistence_save_outbox(&relay, snapshot_ms));
    /*
     * With CONFIG_NVS_DATA_CRC disabled, Zephyr returns zero for this exact
     * duplicate instead of the record length.  An ambiguous-completion retry
     * must still be accepted as durable success.
     */
    zassert_ok(app_mesh_persistence_save_outbox(&relay, snapshot_ms));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, restore_ms));

    restored_retry_ms = restore_ms + (original_retry_ms - snapshot_ms);
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_equal(restored.pending.retry_after_ms, restored_retry_ms);
    zassert_equal(restored.outbox_record.retry_round, 1u);
    zassert_equal(restored.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    zassert_ok(mesh_relay_tick(&restored, restored_retry_ms - 1u, &result));
    zassert_false(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_ok(mesh_relay_tick(&restored, restored_retry_ms, &result));
    zassert_true(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_equal(result.retransmit.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(result.retransmit.next_hop_id, GATEWAY_ID);
    zassert_equal(result.retransmit.payload_len, result_payload_len);
    zassert_mem_equal(result.retransmit.payload,
                      result_payload,
                      result_payload_len);
}

ZTEST(mesh_persistence, test_corrupt_outbox_remains_fail_closed_across_restore)
{
    const uint8_t corrupt[] = {0xA5u, 0x5Au, 0x11u};
    struct mesh_relay restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_persistence_test_write_outbox_raw(
        corrupt, sizeof(corrupt)));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_equal(app_mesh_persistence_restore_outbox(&restored, 2000u),
                  -EINVAL);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_equal(app_mesh_persistence_restore_outbox(&restored, 2100u),
                  -EINVAL);
    zassert_ok(app_mesh_persistence_clear_outbox());
}

ZTEST(mesh_persistence,
      test_inactive_outbox_delete_failure_remains_dirty_and_retryable)
{
    struct mesh_relay occupied;
    struct mesh_relay empty;
    struct mesh_relay restored;
    struct route_candidate route = direct_gateway_route(90u);
    struct app_mesh_persistence_health health = {0};

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_clear_outbox());
    init_deferred_outbox_relay(&occupied, 0x4101u, 1000u);
    zassert_ok(app_mesh_persistence_save_outbox(&occupied, 1100u));

    mesh_relay_init(&empty,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    app_mesh_persistence_test_fail_outbox_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_outbox(&empty, 1200u), -EIO);
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.last_error, -EIO);
    zassert_true(health.consecutive_failures > 0u);

    /*
     * No active RAM outbox means save delegates to the persistent clear. A
     * delete fault must propagate and leave the prior exact owner restorable.
     */
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 1300u));
    zassert_true(mesh_relay_tx_active(&restored));

    zassert_ok(app_mesh_persistence_save_outbox(&empty, 1400u));
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 1500u));
    zassert_false(mesh_relay_tx_active(&restored));
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.consecutive_failures, 0u);
    zassert_equal(health.last_error, 0);
}

ZTEST(mesh_persistence, test_deferred_outbox_transient_read_retains_and_retries)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct route_candidate route = direct_gateway_route(90u);
    int ret;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_deferred_outbox();
    app_mesh_persistence_clear_outbox();
    init_deferred_outbox_relay(&relay, 0x4001u, 1000u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&relay, 1100u));
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);

    /* A reboot or remount starts with an unknown cache.  A failed first read
     * must remain fail-closed until the retry can prove the slot is empty. */
    app_mesh_persistence_test_reset_deferred_presence();
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), -EAGAIN);
    app_mesh_persistence_test_fail_deferred_read(-EIO, 1u);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    ret = app_mesh_persistence_restore_deferred_outbox(&restored, 2000u);
    zassert_equal(ret, -EIO);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), -EAGAIN);

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_deferred_outbox(&restored, 2100u));
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.packet.seq, (uint16_t)(0x4001u + 101u));
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 0);
}

ZTEST(mesh_persistence, test_deferred_outbox_same_snapshot_is_idempotent_and_conflict_busy)
{
    struct mesh_relay first;
    struct mesh_relay same;
    struct mesh_relay different;
    int ret;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_deferred_outbox();
    init_deferred_outbox_relay(&first, 0x4101u, 1000u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&first, 1100u));

    /* A failed delete leaves the old bytes occupied.  Retrying the exact
     * owner must succeed without replacing those bytes. */
    app_mesh_persistence_test_fail_deferred_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_deferred_outbox(), -EIO);
    init_deferred_outbox_relay(&same, 0x4101u, 1200u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&same, 1300u));

    init_deferred_outbox_relay(&different, 0x4102u, 1400u);
    ret = app_mesh_persistence_save_deferred_outbox(&different, 1500u);
    zassert_equal(ret, -EBUSY);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);

    mesh_relay_init(&same,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_deferred_outbox(&same, 1600u));
    zassert_equal(same.pending.packet.seq, (uint16_t)(0x4101u + 101u));
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 0);
}

ZTEST(mesh_persistence, test_deferred_outbox_contention_is_retryable)
{
    struct mesh_relay relay;
    struct mesh_relay restored;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_deferred_outbox();
    init_deferred_outbox_relay(&relay, 0x4151u, 1000u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&relay, 1100u));

    /* A competing read/write must never observe or publish a half-transaction. */
    app_mesh_persistence_test_set_deferred_busy(true);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), -EBUSY);
    zassert_equal(app_mesh_persistence_clear_deferred_outbox(), -EBUSY);
    zassert_equal(app_mesh_persistence_save_deferred_outbox(&relay, 1200u),
                  -EBUSY);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_equal(app_mesh_persistence_restore_deferred_outbox(&restored,
                                                                 1300u),
                  -EBUSY);
    app_mesh_persistence_test_set_deferred_busy(false);

    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);
    zassert_ok(app_mesh_persistence_clear_deferred_outbox());
}

ZTEST(mesh_persistence,
      test_deferred_outbox_corruption_remains_fail_closed_across_restore)
{
    struct mesh_relay relay;
    struct mesh_relay_outbox_snapshot snapshot;
    int ret;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_deferred_outbox();
    init_deferred_outbox_relay(&relay, 0x4201u, 1000u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&relay, 1100u));
    zassert_ok(mesh_relay_export_outbox_snapshot(&relay, 1100u, &snapshot));
    snapshot.version++;
    zassert_ok(app_mesh_persistence_test_write_deferred_outbox_snapshot(
        &snapshot,
        sizeof(snapshot)));

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    ret = app_mesh_persistence_restore_deferred_outbox(&relay, 2000u);
    zassert_equal(ret, -EBADMSG);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    ret = app_mesh_persistence_restore_deferred_outbox(&relay, 2100u);
    zassert_equal(ret, -EBADMSG);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);
    zassert_ok(app_mesh_persistence_clear_deferred_outbox());
}

ZTEST(mesh_persistence, test_deferred_outbox_promotion_keeps_dual_copies_until_cleanup)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct route_candidate route = direct_gateway_route(90u);

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_deferred_outbox();
    app_mesh_persistence_clear_outbox();
    init_deferred_outbox_relay(&relay, 0x4301u, 1000u);
    zassert_ok(app_mesh_persistence_save_deferred_outbox(&relay, 1100u));

    app_mesh_persistence_test_fail_outbox_write(-EIO, 1u);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_equal(app_mesh_persistence_restore_deferred_outbox(&restored, 2000u),
                  -EIO);
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);

    app_mesh_persistence_test_fail_deferred_delete(-EIO, 1u);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_equal(app_mesh_persistence_restore_deferred_outbox(&restored, 2100u),
                  -EIO);
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 1);

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_deferred_outbox(&restored, 2200u));
    zassert_equal(app_mesh_persistence_deferred_outbox_present(), 0);
}

ZTEST(mesh_persistence, test_click_preemption_saves_collection_outbox_for_retry_restore)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 0x1019u,
        .node_id = LOCAL_ID,
        .node_boot_counter = 87u,
        .result_seq = 88u,
    };
    struct route_candidate route = direct_gateway_route(90u);
    struct proto_packet packet = {0};
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_click_preempt_plan plan;
    struct app_mesh_click_preempt_result preempt_result;
    struct preempt_save_ctx save_ctx;
    struct app_mesh_click_preempt_ops ops;
    const uint32_t start_ms = 5000u;
    const uint32_t preempt_ms = 5033u;
    const uint32_t restore_ms = 7000u;
    uint32_t restored_retry_ms;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_outbox();
    app_mesh_persistence_clear_deferred_outbox();
    (void)k_work_cancel_delayable(&test_tx_timeout_work);

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3019u,
                                            &result_payload_len);
    zassert_ok(mesh_init_command_result(&packet,
                                        LOCAL_ID,
                                        GATEWAY_ID,
                                        result_id.command_seq,
                                        result_id.result_seq,
                                        (uint8_t)result_payload_len,
                                        false));

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&relay,
                                   &packet,
                                   result_payload,
                                   result_payload_len,
                                   start_ms,
                                   &tx));
    zassert_equal(relay.pending.state, MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    zassert_equal(relay.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    zassert_ok(mesh_prepare_click_preemption(&relay,
                                             LOCAL_ID,
                                             preempt_ms,
                                             &plan));
    zassert_true(plan.save_outbox);
    zassert_true(plan.schedule_timeout);
    zassert_false(plan.clear_outbox);
    zassert_false(plan.cancel_timeout);
    zassert_true(plan.cancel_active_tx);
    zassert_equal(relay.pending.state, MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    save_ctx = (struct preempt_save_ctx) {
        .relay = &relay,
        .now_ms = preempt_ms,
    };
    ops = (struct app_mesh_click_preempt_ops) {
        .save_deferred_outbox = save_deferred_outbox_for_preempt,
        .schedule_timeout = schedule_timeout_for_preempt,
        .cancel_active_tx = cancel_active_tx_for_preempt,
        .ctx = &save_ctx,
    };
    zassert_ok(app_mesh_apply_click_preempt_plan(&plan, &ops, &preempt_result));
    zassert_true(preempt_result.outbox_saved);
    zassert_true(preempt_result.timeout_scheduled);
    zassert_true(preempt_result.active_tx_cancelled);
    zassert_true(preempt_result.transaction_committed);
    zassert_equal(preempt_result.custody_owner,
                  APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_OUTBOX);
    zassert_false(preempt_result.outbox_cleared);
    zassert_false(preempt_result.timeout_cancelled);
    zassert_equal(save_ctx.save_count, 1u);
    zassert_equal(save_ctx.schedule_count, 1u);
    zassert_false(mesh_relay_tx_active(&relay));
    zassert_true(k_work_delayable_is_pending(&test_tx_timeout_work));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_deferred_outbox(&restored,
                                                             restore_ms));

    restored_retry_ms = restore_ms + RELAY_BUSY_RETRY_MIN_MS;
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_equal(restored.pending.retry_after_ms, restored_retry_ms);
    zassert_equal(restored.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    zassert_equal(restored.upstream.candidates[0].failure_count, 0u);
    zassert_equal(restored.upstream.candidates[0].hold_down_until_ms, 0u);

    zassert_ok(mesh_relay_tick(&restored, restored_retry_ms - 1u, &result));
    zassert_false(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_ok(mesh_relay_tick(&restored, restored_retry_ms, &result));
    zassert_true(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_equal(result.retransmit.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(result.retransmit.next_hop_id, GATEWAY_ID);
    zassert_equal(result.retransmit.payload_len, result_payload_len);
    zassert_mem_equal(result.retransmit.payload,
                      result_payload,
                      result_payload_len);
}

ZTEST(mesh_persistence, test_forwarded_child_result_payload_outbox_round_trip_after_grant)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 0x20212225u,
        .node_id = CHILD_ID,
        .node_boot_counter = 43u,
        .result_seq = 46u,
    };
    const struct result_grant grant = {
        .result_id = result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = 64u,
        .event_offset_hint = 0u,
    };
    struct route_candidate route = direct_gateway_route(90u);
    struct proto_packet result_packet = {0};
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .src_id = GATEWAY_ID,
        .dst_id = LOCAL_ID,
        .session_id = result_id.command_seq,
        .seq = 15u,
        .ttl = 1u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound offer_tx;
    struct mesh_relay_result grant_result;
    struct mesh_relay_result tick_result;
    uint8_t result_payload[96];
    uint8_t grant_payload[96];
    size_t result_payload_len = 0u;
    size_t grant_payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_outbox();

    build_identity_command_result_payload(result_payload,
                                          sizeof(result_payload),
                                          64u,
                                          &result_id,
                                          &result_payload_len);
    zassert_ok(mesh_init_command_result(&result_packet,
                                        CHILD_ID,
                                        GATEWAY_ID,
                                        result_id.command_seq,
                                        result_id.result_seq,
                                        (uint8_t)result_payload_len,
                                        false));
    zassert_ok(result_grant_append_tlvs(grant_payload,
                                        sizeof(grant_payload),
                                        &grant_payload_len,
                                        &grant));
    grant_packet.payload_len = (uint16_t)grant_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_result_offer(&relay,
                                             &result_packet,
                                             result_payload,
                                             result_payload_len,
                                             4400u,
                                             &offer_tx));
    zassert_ok(mesh_relay_handle_rx(&relay,
                                    &grant_packet,
                                    grant_payload,
                                    grant_payload_len,
                                    GATEWAY_ID,
                                    80u,
                                    4410u,
                                    &grant_result));
    zassert_true(has_action(&grant_result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_false(relay.pending.result_offer_active);
    zassert_equal(relay.pending.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(relay.pending.packet.src_id, CHILD_ID);
    zassert_equal(relay.pending.packet.dst_id, GATEWAY_ID);
    zassert_equal(relay.pending.state, MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    zassert_equal(relay.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    zassert_ok(app_mesh_persistence_save_outbox(&relay, 4420u));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 5000u));

    zassert_false(restored.pending.result_offer_active);
    zassert_equal(restored.pending.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(restored.pending.packet.src_id, CHILD_ID);
    zassert_equal(restored.pending.packet.dst_id, GATEWAY_ID);
    zassert_equal(restored.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_equal(restored.outbox_record.delivery_state,
                  MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);

    zassert_ok(mesh_relay_tick(&restored,
                               5000u + RELAY_BUSY_RETRY_MIN_MS,
                               &tick_result));
    zassert_true(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    zassert_equal(tick_result.retransmit.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(tick_result.retransmit.packet.src_id, CHILD_ID);
    zassert_equal(tick_result.retransmit.packet.dst_id, GATEWAY_ID);
    zassert_equal(tick_result.retransmit.next_hop_id, GATEWAY_ID);
    zassert_equal(tick_result.retransmit.payload_len, result_payload_len);
    zassert_mem_equal(tick_result.retransmit.payload,
                      result_payload,
                      result_payload_len);
}

ZTEST(mesh_persistence, test_child_custody_reservation_round_trip_and_clear)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY_ID,
            .gateway_epoch = 13u,
            .command_seq = 0x22334455u,
            .node_id = CHILD_ID,
            .node_boot_counter = 21u,
            .result_seq = 22u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x789au,
        .result_digest = { 0x78u, 0x9au },
        .priority = 4u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = CHILD_ID,
        .dst_id = LOCAL_ID,
        .session_id = offer.result_id.command_seq,
        .seq = offer.result_id.result_seq,
        .ttl = 1u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_child_custody();

    zassert_ok(result_offer_append_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &offer));
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(mesh_relay_handle_rx(&relay,
                                    &packet,
                                    payload,
                                    payload_len,
                                    CHILD_ID,
                                    80u,
                                    4300u,
                                    &result));
    zassert_true(relay.result_offer_reservation.valid);
    zassert_ok(app_mesh_persistence_save_child_custody(&relay, 4301u));

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(app_mesh_persistence_restore_child_custody(&restored, 5000u));
    zassert_true(restored.result_offer_reservation.valid);
    zassert_equal(restored.result_offer_reservation.child_id, CHILD_ID);
    assert_result_id_equal(&restored.result_offer_reservation.result_id,
                           &offer.result_id);
    zassert_equal(restored.result_offer_reservation.result_len, offer.result_len);
    zassert_mem_equal(restored.result_offer_reservation.result_digest,
                      offer.result_digest,
                      sizeof(offer.result_digest));

    app_mesh_persistence_clear_child_custody();
    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(app_mesh_persistence_restore_child_custody(&restored, 6000u));
    zassert_false(restored.result_offer_reservation.valid);
}

ZTEST(mesh_persistence, test_result_grant_send_failure_restores_reservation_for_retry)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY_ID,
            .gateway_epoch = 13u,
            .command_seq = 0x22334466u,
            .node_id = CHILD_ID,
            .node_boot_counter = 23u,
            .result_seq = 24u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x55aau,
        .result_digest = { 0x55u, 0xaau },
        .priority = 4u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = CHILD_ID,
        .dst_id = LOCAL_ID,
        .session_id = offer.result_id.command_seq,
        .seq = offer.result_id.result_seq,
        .ttl = 1u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_result first_result;
    struct mesh_relay_result retry_result;
    struct result_grant_send_ctx ctx;
    struct app_mesh_result_handoff_ops ops;
    struct app_mesh_result_handoff_status status;
    uint8_t payload[96];
    size_t payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_child_custody();

    zassert_ok(result_offer_append_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &offer));
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(mesh_relay_handle_rx(&relay,
                                    &packet,
                                    payload,
                                    payload_len,
                                    CHILD_ID,
                                    80u,
                                    4300u,
                                    &first_result));
    zassert_true(has_action(&first_result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    zassert_false(has_action(&first_result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    zassert_true(relay.result_offer_reservation.valid);

    ctx = (struct result_grant_send_ctx) {
        .relay = &relay,
        .now_ms = 4301u,
        .send_ret = -ENOTCONN,
    };
    ops = (struct app_mesh_result_handoff_ops) {
        .save_child_custody = save_child_custody_for_grant,
        .send_result_grant = send_result_grant_for_test,
        .note_tx_sent = note_tx_sent_for_grant,
        .ctx = &ctx,
    };
    app_mesh_result_handoff_result_grant(&first_result, true, &ops, &status);
    zassert_true(status.child_custody_saved);
    zassert_true(status.child_custody_ready);
    zassert_false(status.result_grant_sent);
    zassert_false(status.result_grant_suppressed);
    zassert_equal(status.send_ret, -ENOTCONN);
    zassert_equal(ctx.save_count, 1u);
    zassert_equal(ctx.send_count, 1u);
    zassert_equal(ctx.note_count, 0u);
    zassert_true(relay.result_offer_reservation.valid);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(app_mesh_persistence_restore_child_custody(&restored, 5000u));
    zassert_true(restored.result_offer_reservation.valid);
    zassert_equal(restored.result_offer_reservation.child_id, CHILD_ID);
    assert_result_id_equal(&restored.result_offer_reservation.result_id,
                           &offer.result_id);

    zassert_ok(mesh_relay_handle_rx(&restored,
                                    &packet,
                                    payload,
                                    payload_len,
                                    CHILD_ID,
                                    80u,
                                    5001u,
                                    &retry_result));
    zassert_true(has_action(&retry_result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    zassert_false(has_action(&retry_result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    zassert_false(has_action(&retry_result, MESH_RELAY_ACTION_DROP));
    zassert_true(restored.result_offer_reservation.valid);
}

ZTEST(mesh_persistence, test_child_result_bundle_round_trip_and_flush_after_restore)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = 13u,
        .command_seq = 0x33445566u,
        .node_id = CHILD_ID,
        .node_boot_counter = 31u,
        .result_seq = 32u,
    };
    struct route_candidate route = direct_gateway_route(90u);
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = CHILD_ID,
        .dst_id = GATEWAY_ID,
        .session_id = 92u,
        .seq = 8u,
        .ttl = MESH_DEFAULT_TTL,
        .message_age_ms = 10u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_result result;
    struct result_bundle_header bundle;
    uint8_t payload[96];
    size_t payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_child_custody();

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            3012u,
                                            &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_handle_rx(&relay,
                                    &packet,
                                    payload,
                                    payload_len,
                                    CHILD_ID,
                                    90u,
                                    1000u,
                                    &result));
    zassert_true(mesh_relay_result_bundle_pending(&relay));
    zassert_ok(app_mesh_persistence_save_child_custody(&relay, 1010u));

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, LOCAL_ID, GATEWAY_ID, 13u);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(app_mesh_persistence_restore_child_custody(&restored, 2000u));
    zassert_true(mesh_relay_result_bundle_pending(&restored));
    zassert_equal(mesh_relay_result_bundle_due_ms(&restored), 2015u);

    zassert_ok(mesh_relay_tick(&restored, 2014u, &result));
    zassert_false(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    zassert_ok(mesh_relay_tick(&restored, 2015u, &result));
    zassert_true(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    zassert_equal(result.forward.packet.msg_type, MSG_RESULT_BUNDLE);
    zassert_equal(result.forward.packet.message_age_ms, 35u);
    zassert_ok(result_bundle_header_from_tlvs(result.forward.payload,
                                              result.forward.payload_len,
                                              &bundle));
    zassert_equal(bundle.record_count, 1u);
    mesh_relay_result_bundle_note_forwarded(&restored, &result.forward);
    zassert_false(mesh_relay_result_bundle_pending(&restored));
}

ZTEST(mesh_persistence,
      test_corrupt_child_custody_remains_fail_closed_across_restore)
{
    const uint8_t corrupt[] = {0xC3u, 0x3Cu, 0x77u};
    struct mesh_relay restored;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_child_custody();
    zassert_ok(app_mesh_persistence_test_write_child_custody_raw(
        corrupt, sizeof(corrupt)));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_equal(
        app_mesh_persistence_restore_child_custody(&restored, 3000u),
        -EINVAL);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_equal(
        app_mesh_persistence_restore_child_custody(&restored, 3100u),
        -EINVAL);
    zassert_ok(app_mesh_persistence_clear_child_custody());
}

ZTEST(mesh_persistence,
      test_child_custody_delete_failure_remains_retryable)
{
    struct mesh_relay empty;
    struct app_mesh_persistence_health health = {0};
    const uint8_t retained[] = {0xC3u, 0x3Cu, 0x77u};

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_child_custody());
    zassert_ok(app_mesh_persistence_test_write_child_custody_raw(
        retained, sizeof(retained)));
    mesh_relay_init(&empty,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);

    app_mesh_persistence_test_fail_child_custody_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_child_custody(&empty, 3200u),
                  -EIO);
    app_mesh_persistence_get_health(&health);
    zassert_equal(health.last_error, -EIO);
    zassert_true(health.consecutive_failures > 0u);

    /*
     * The failed deletion must leave the old key observable; otherwise the
     * caller would incorrectly mark custody persistence clean.
     */
    zassert_equal(
        app_mesh_persistence_restore_child_custody(&empty, 3300u),
        -EINVAL);
    zassert_ok(app_mesh_persistence_save_child_custody(&empty, 3400u));
    zassert_ok(app_mesh_persistence_restore_child_custody(&empty, 3500u));
}

ZTEST_SUITE(mesh_persistence,
            NULL,
            mesh_persistence_suite_setup,
            NULL,
            NULL,
            NULL);

static int main_init(void)
{
    k_work_init_delayable(&test_tx_timeout_work, timeout_handler);
    return 0;
}

SYS_INIT(main_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
