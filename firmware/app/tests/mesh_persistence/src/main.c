#include "app_mesh_persistence.h"
#include "mesh.h"
#include "protocol.h"

#include <string.h>
#include <zephyr/ztest.h>

#define LOCAL_ID 0x1111222233334444ull
#define GATEWAY_ID 0x9999888877776666ull
#define CHILD_ID 0x5555666677778888ull

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
