#include "app_mesh_persistence.h"
#include "app_mesh_preemption.h"
#include "app_mesh_result_handoff.h"
#include "mesh.h"
#include "protocol.h"
#include "route.h"

#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define LOCAL_ID 0x1111222233334444ull
#define GATEWAY_ID 0x9999888877776666ull
#define CHILD_ID 0x5555666677778888ull
#define MEMBER_A_ID 0x0102030405060708ull
#define MEMBER_B_ID 0x1112131415161718ull
#define MEMBER_C_ID 0x2122232425262728ull

static struct k_work_delayable test_tx_timeout_work;

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
    return NULL;
}

int app_mesh_persistence_test_write_gateway_membership_snapshot(
    const void *snapshot,
    size_t snapshot_len);

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

static int save_outbox_for_preempt(void *opaque)
{
    struct preempt_save_ctx *ctx = opaque;

    ctx->save_count++;
    return app_mesh_persistence_save_outbox(ctx->relay, ctx->now_ms);
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

static void assert_membership_roster_equal(
    const struct gateway_membership_roster *actual,
    const struct gateway_membership_roster *expected)
{
    zassert_equal(actual->valid, expected->valid);
    zassert_equal(actual->membership_epoch, expected->membership_epoch);
    zassert_equal(actual->node_count, expected->node_count);
    zassert_mem_equal(actual->node_ids,
                      expected->node_ids,
                      expected->node_count * sizeof(expected->node_ids[0]));
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

ZTEST(mesh_persistence, test_collection_result_snapshot_round_trip_and_clear)
{
    struct app_mesh_collection_result_snapshot saved = make_snapshot();
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_collection_result();

    zassert_ok(app_mesh_persistence_save_collection_result(&saved));
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    app_mesh_persistence_clear_collection_result();
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_false(restored.valid);
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
    app_mesh_persistence_clear_gateway_eack_custody();
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

    app_mesh_persistence_clear_gateway_eack_custody();
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_eack_custody(&restored));
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence, test_gateway_eack_custody_rejects_corruption)
{
    struct gateway_collection_eack_custody_snapshot snapshot = {0};

    zassert_ok(app_mesh_persistence_init());
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
}

ZTEST(mesh_persistence, test_gateway_membership_snapshot_round_trip_and_clear)
{
    struct gateway_membership_roster saved = make_gateway_membership_roster();
    struct gateway_membership_roster restored;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());

    zassert_ok(app_mesh_persistence_save_gateway_membership(&saved));

    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(&restored));
    assert_membership_roster_equal(&restored, &saved);

    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_gateway_membership(&restored));
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

ZTEST(mesh_persistence, test_gateway_membership_restore_rejects_and_clears_bad_nvs)
{
    struct gateway_membership_roster roster = make_gateway_membership_roster();
    struct gateway_membership_roster restored;
    struct gateway_membership_snapshot snapshot;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_gateway_membership());

    zassert_ok(gateway_membership_export_snapshot(&roster, &snapshot));
    snapshot.version++;
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &snapshot,
        sizeof(snapshot)));

    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(app_mesh_persistence_restore_gateway_membership(&restored), -EINVAL);
    zassert_false(restored.valid);
    zassert_ok(app_mesh_persistence_restore_gateway_membership(&restored));
    zassert_false(restored.valid);

    zassert_ok(gateway_membership_export_snapshot(&roster, &snapshot));
    zassert_ok(app_mesh_persistence_clear_gateway_membership());
    zassert_ok(app_mesh_persistence_test_write_gateway_membership_snapshot(
        &snapshot,
        sizeof(snapshot) - 1u));

    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(app_mesh_persistence_restore_gateway_membership(&restored), -EINVAL);
    zassert_false(restored.valid);
    zassert_ok(app_mesh_persistence_restore_gateway_membership(&restored));
    zassert_false(restored.valid);
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
        .save_outbox = save_outbox_for_preempt,
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
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, restore_ms));

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
        .session_id = 98u,
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
                                        98u,
                                        15u,
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
        .priority = 4u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = CHILD_ID,
        .dst_id = LOCAL_ID,
        .session_id = 91u,
        .seq = 7u,
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
    zassert_equal(restored.result_offer_reservation.result_crc, offer.result_crc);

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
        .priority = 4u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = CHILD_ID,
        .dst_id = LOCAL_ID,
        .session_id = 93u,
        .seq = 9u,
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
