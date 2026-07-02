#include "app_mesh_persistence.h"
#include "mesh.h"
#include "protocol.h"
#include "route.h"

#include <string.h>
#include <zephyr/ztest.h>

#define LOCAL_ID 0x1111222233334444ull
#define GATEWAY_ID 0x9999888877776666ull
#define CHILD_ID 0x5555666677778888ull

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

ZTEST_SUITE(mesh_persistence, NULL, NULL, NULL, NULL, NULL);
