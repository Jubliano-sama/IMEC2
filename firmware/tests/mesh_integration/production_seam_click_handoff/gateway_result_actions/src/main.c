#include "app_gateway_collection_recovery.h"
#include "app_mesh_result_handoff.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"
#include "semantic_digest.h"

#include <zephyr/ztest.h>

#include <string.h>

#define TEST_GATEWAY_ID UINT64_C(0x0102030405060708)
#define TEST_ANCHOR_A_ID UINT64_C(0x1112131415161718)
#define TEST_ANCHOR_B_ID UINT64_C(0x2122232425262728)
#define TEST_GATEWAY_EPOCH 3u
#define TEST_RESULT_LEN 96u

struct result_fixture {
    struct command_result_id result_id;
    struct result_offer offer;
    struct proto_packet offer_packet;
    struct proto_packet result_packet;
    uint8_t offer_payload[96];
    size_t offer_payload_len;
    uint8_t result_payload[TEST_RESULT_LEN];
    size_t result_payload_len;
};

struct send_capture {
    struct mesh_outbound sent;
    uint32_t send_count;
    uint32_t note_count;
};

static bool has_action(const struct mesh_relay_result *result,
                       uint32_t action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static void build_result_payload(struct result_fixture *fixture)
{
    uint8_t padding[24];

    fixture->result_payload_len = 0u;
    zassert_ok(command_result_id_append_tlvs(fixture->result_payload,
                                             sizeof(fixture->result_payload),
                                             &fixture->result_payload_len,
                                             &fixture->result_id));
    zassert_ok(mesh_append_command_result(fixture->result_payload,
                                          sizeof(fixture->result_payload),
                                          &fixture->result_payload_len,
                                          CMD_GET_STATUS,
                                          COMMAND_OK,
                                          0u));
    zassert_ok(tlv_append_u32(fixture->result_payload,
                              sizeof(fixture->result_payload),
                              &fixture->result_payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              UINT32_C(0x10203040)));
    memset(padding, 0xa5, sizeof(padding));
    while (fixture->result_payload_len < sizeof(fixture->result_payload)) {
        const size_t remaining =
            sizeof(fixture->result_payload) - fixture->result_payload_len;
        const uint8_t chunk_len =
            remaining > sizeof(padding) + 2u ?
                (uint8_t)sizeof(padding) :
                (uint8_t)(remaining > 2u ? remaining - 2u : 0u);

        zassert_ok(tlv_append_bytes(fixture->result_payload,
                                    sizeof(fixture->result_payload),
                                    &fixture->result_payload_len,
                                    TLV_MESH_TEST_PADDING,
                                    padding,
                                    chunk_len));
    }
    zassert_equal(fixture->result_payload_len, TEST_RESULT_LEN);
}

static void result_fixture_init(struct result_fixture *fixture,
                                uint64_t anchor_id,
                                uint32_t command_seq,
                                uint16_t result_seq)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->result_id = (struct command_result_id) {
        .gateway_id = TEST_GATEWAY_ID,
        .gateway_epoch = TEST_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .node_id = anchor_id,
        .node_boot_counter = 7u,
        .result_seq = result_seq,
    };
    build_result_payload(fixture);
    fixture->offer = (struct result_offer) {
        .result_id = fixture->result_id,
        .result_len = (uint16_t)fixture->result_payload_len,
        .result_crc = proto_crc16_ccitt_false(fixture->result_payload,
                                              fixture->result_payload_len),
        .priority = 4u,
    };
    zassert_true(semantic_digest_sha256(fixture->result_payload,
                                        fixture->result_payload_len,
                                        fixture->offer.result_digest));
    zassert_ok(result_offer_append_tlvs(fixture->offer_payload,
                                        sizeof(fixture->offer_payload),
                                        &fixture->offer_payload_len,
                                        &fixture->offer));
    fixture->offer_packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = anchor_id,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = command_seq,
        .seq = result_seq,
        .ttl = 1u,
        .payload_len = (uint16_t)fixture->offer_payload_len,
    };
    zassert_ok(mesh_init_command_result(&fixture->result_packet,
                                        anchor_id,
                                        TEST_GATEWAY_ID,
                                        command_seq,
                                        result_seq,
                                        (uint8_t)fixture->result_payload_len,
                                        false));
}

static int capture_send(const struct mesh_outbound *out, void *ctx)
{
    struct send_capture *capture = ctx;

    zassert_not_null(out);
    zassert_not_null(capture);
    capture->sent = *out;
    capture->send_count++;
    return 0;
}

static void capture_note(const struct mesh_outbound *out, void *ctx)
{
    struct send_capture *capture = ctx;

    zassert_not_null(out);
    zassert_not_null(capture);
    capture->note_count++;
}

static void gateway_init(struct mesh_relay *gateway,
                         struct mesh_gateway_ack_store *ack_store)
{
    memset(ack_store, 0, sizeof(*ack_store));
    mesh_relay_init(gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    zassert_ok(mesh_relay_attach_gateway_ack_store(gateway, ack_store));
}

static struct route_candidate source_gateway_route(uint32_t epoch)
{
    return (struct route_candidate) {
        .next_hop_id = TEST_GATEWAY_ID,
        .gateway_id = TEST_GATEWAY_ID,
        .route_epoch = epoch,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = 90u,
        .valid = true,
    };
}

ZTEST(production_seam_gateway_result_actions,
      test_direct_large_result_offer_grants_and_terminal_receive_acks)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct result_fixture fixture;
    struct mesh_relay_result offer_result;
    struct mesh_relay_result receive_result;
    struct mesh_relay_result commit_result;
    struct send_capture capture = {0};
    const struct app_mesh_result_handoff_ops ops = {
        .send_result_grant = capture_send,
        .note_tx_sent = capture_note,
        .ctx = &capture,
    };
    struct app_mesh_result_handoff_status handoff_status;

    gateway_init(&gateway, &ack_store);
    result_fixture_init(&fixture,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x22334455),
                        22u);

    zassert_ok(mesh_relay_handle_rx(&gateway,
                                    &fixture.offer_packet,
                                    fixture.offer_payload,
                                    fixture.offer_payload_len,
                                    TEST_ANCHOR_A_ID,
                                    90u,
                                    4300u,
                                    &offer_result));
    zassert_true(has_action(&offer_result,
                            MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    zassert_false(has_action(&offer_result,
                             MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    zassert_true(gateway.result_offer_reservation.valid);

    app_mesh_result_handoff_result_grant(&offer_result,
                                         &ops,
                                         &handoff_status);
    zassert_true(handoff_status.result_grant_sent);
    zassert_equal(capture.send_count, 1u);
    zassert_equal(capture.note_count, 1u);
    zassert_equal(capture.sent.packet.msg_type, MSG_RESULT_GRANT);
    zassert_equal(capture.sent.packet.dst_id, TEST_ANCHOR_A_ID);

    zassert_ok(mesh_relay_handle_rx(&gateway,
                                    &fixture.result_packet,
                                    fixture.result_payload,
                                    fixture.result_payload_len,
                                    TEST_ANCHOR_A_ID,
                                    90u,
                                    4310u,
                                    &receive_result));
    zassert_true(has_action(&receive_result,
                            MESH_RELAY_ACTION_DELIVER_LOCAL),
                 "status=%d actions=0x%08x",
                 receive_result.status,
                 receive_result.actions);
    zassert_false(has_action(&receive_result,
                             MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    zassert_ok(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &fixture.result_packet,
                                                  fixture.result_payload,
                                                  fixture.result_payload_len,
                                                  TEST_ANCHOR_A_ID,
                                                  4311u,
                                                  &commit_result));
    zassert_true(has_action(&commit_result,
                            MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    zassert_equal(commit_result.gateway_ack.packet.msg_type,
                  MSG_GATEWAY_ACK);
    zassert_equal(commit_result.gateway_ack.next_hop_id,
                  TEST_ANCHOR_A_ID);
}

ZTEST(production_seam_gateway_result_actions,
      test_competing_direct_offer_returns_busy_and_nonanchor_hop_ack_survives)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct result_fixture first;
    struct result_fixture second;
    struct mesh_relay_result first_result;
    struct mesh_relay_result second_result;
    struct mesh_relay_result hop_result = {
        .actions = MESH_RELAY_ACTION_SEND_HOP_ACK |
                   MESH_RELAY_ACTION_CUSTODY_ACCEPTED,
    };
    struct mesh_relay_result unretained_forward = {
        .actions = MESH_RELAY_ACTION_FORWARD |
                   MESH_RELAY_ACTION_SEND_HOP_ACK,
    };
    struct app_mesh_result_handoff_status handoff_status;

    gateway_init(&gateway, &ack_store);
    result_fixture_init(&first,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x33445566),
                        32u);
    result_fixture_init(&second,
                        TEST_ANCHOR_B_ID,
                        UINT32_C(0x44556677),
                        33u);

    zassert_ok(mesh_relay_handle_rx(&gateway,
                                    &first.offer_packet,
                                    first.offer_payload,
                                    first.offer_payload_len,
                                    TEST_ANCHOR_A_ID,
                                    90u,
                                    5000u,
                                    &first_result));
    zassert_true(has_action(&first_result,
                            MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    zassert_ok(mesh_relay_handle_rx(&gateway,
                                    &second.offer_packet,
                                    second.offer_payload,
                                    second.offer_payload_len,
                                    TEST_ANCHOR_B_ID,
                                    90u,
                                    5001u,
                                    &second_result));
    zassert_true(has_action(&second_result,
                            MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    zassert_false(has_action(&second_result,
                             MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    zassert_equal(second_result.busy.packet.msg_type, MSG_RESULT_BUSY);
    zassert_equal(second_result.busy.next_hop_id, TEST_ANCHOR_B_ID);

    app_mesh_result_handoff_prepare_hop_ack(&unretained_forward,
                                            false,
                                            NULL,
                                            &handoff_status);
    zassert_false(handoff_status.hop_ack_allowed,
                  "unretained child response received a false custody ACK");

    app_mesh_result_handoff_prepare_hop_ack(&hop_result,
                                            false,
                                            NULL,
                                            &handoff_status);
    zassert_true(handoff_status.hop_ack_allowed,
                 "non-anchor custody lost its hop ACK action");
}

ZTEST(production_seam_gateway_result_actions,
      test_rebooted_gateway_releases_stale_result_only_through_recovery_eack)
{
    struct app_gateway_collection_recovery recovery = {0};
    struct mesh_relay source;
    struct mesh_relay_result source_result;
    struct mesh_outbound source_tx;
    struct mesh_outbound recovery_eack;
    struct result_fixture fixture;
    const struct route_candidate route = source_gateway_route(
        TEST_GATEWAY_EPOCH);

    /* The source retains this original collection result while the old
     * gateway's RAM-only collection ledger disappears on reset.  The fresh
     * recovery owner begins empty; preflight alone cannot affect custody. */
    result_fixture_init(&fixture,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x55667788),
                        44u);
    mesh_relay_init(&source,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    zassert_ok(route_upsert_candidate(&source.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&source,
                                   &fixture.result_packet,
                                   fixture.result_payload,
                                   fixture.result_payload_len,
                                   7000u,
                                   &source_tx));
    zassert_true(mesh_relay_tx_active(&source));
    zassert_false(recovery.active);

    zassert_ok(app_gateway_collection_recovery_preflight(
        &fixture.result_packet,
        fixture.result_payload,
        fixture.result_payload_len,
        TEST_GATEWAY_ID,
        TEST_GATEWAY_EPOCH + 1u));
    zassert_ok(app_gateway_collection_recovery_reserve_host_custody(
        &recovery,
        &fixture.result_packet,
        fixture.result_payload,
        fixture.result_payload_len,
        TEST_GATEWAY_ID,
        TEST_GATEWAY_EPOCH + 1u));
    zassert_false(recovery.active,
                  "admission before the exact host receipt must not emit EACK");
    zassert_true(recovery.host_custody_pending,
                 "the retained stale head must block a fresh collection");
    zassert_true(mesh_relay_tx_active(&source),
                 "source custody must survive host admission alone");

    /* This call represents the exact GUI receipt of the retained BLE head. */
    zassert_ok(app_gateway_collection_recovery_begin(
        &recovery,
        &fixture.result_packet,
        fixture.result_payload,
        fixture.result_payload_len,
        TEST_GATEWAY_ID,
        TEST_GATEWAY_EPOCH + 1u,
        UINT32_C(0x12340001)));
    zassert_ok(app_gateway_collection_recovery_outbound(&recovery,
                                                         &recovery_eack));
    zassert_equal(recovery_eack.packet.msg_type,
                  MSG_GATEWAY_COLLECTION_EACK);
    zassert_equal(recovery_eack.packet.dst_id, MESH_BROADCAST_ID);
    zassert_equal(recovery_eack.packet.seq, 1u);
    zassert_true(mesh_relay_tx_active(&source),
                 "freezing a recovery EACK must not clear source custody");

    zassert_ok(mesh_relay_handle_rx(&source,
                                    &recovery_eack.packet,
                                    recovery_eack.payload,
                                    recovery_eack.payload_len,
                                    TEST_GATEWAY_ID,
                                    90u,
                                    7010u,
                                    &source_result));
    zassert_true(has_action(&source_result,
                            MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    zassert_false(mesh_relay_tx_active(&source),
                  "only the exact closed recovery EACK may release source custody");
}

ZTEST_SUITE(production_seam_gateway_result_actions,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
