#include "app_click_event_sequence.h"
#include "app_mesh_persistence.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"

#include "posix_board_if.h"

#include <zephyr/sys/printk.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define CLICKER_ID UINT64_C(0x1111222233334444)
#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define TEST_SESSION_ID UINT32_C(0x11223344)
#define TEST_PACKET_SEQ 7u

static const uint8_t self_test_payload[] = {
    0x44u, 0x55u, 0x66u, 0x77u,
};

static void fail(int code, const char *reason, int detail)
{
    printk("SHARED_NVS failure=%s detail=%d\n", reason, detail);
    posix_exit(code);
}

static struct route_candidate direct_gateway_route(uint32_t now_ms)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 1u,
        .last_seen_ms = now_ms,
        .hop_count = 0u,
        .link_quality = 90u,
        .valid = true,
    };
}

static int build_gateway_ack(
    struct proto_packet *ack,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    uint16_t ack_seq)
{
    int ret;

    *payload_len = 0u;
    ret = mesh_append_requested_seq(payload,
                                    payload_cap,
                                    payload_len,
                                    acknowledged_packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_ack_semantic_identity(payload,
                                            payload_cap,
                                            payload_len,
                                            acknowledged_packet,
                                            acknowledged_payload,
                                            acknowledged_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_init_gateway_ack(ack,
                                 GATEWAY_ID,
                                 CLICKER_ID,
                                 acknowledged_packet->session_id,
                                 ack_seq,
                                 (uint8_t)*payload_len);
}

static int original_save(struct mesh_relay *relay, uint32_t now_ms)
{
    struct proto_packet packet = {
        .msg_type = MSG_SELF_TEST_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = CLICKER_ID,
        .dst_id = GATEWAY_ID,
        .session_id = TEST_SESSION_ID,
        .seq = TEST_PACKET_SEQ,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(self_test_payload),
    };
    struct mesh_outbound tx;
    int ret;

    ret = mesh_relay_start_tx(relay,
                              &packet,
                              self_test_payload,
                              sizeof(self_test_payload),
                              now_ms,
                              &tx);
    if (ret != PROTO_OK) {
        return ret;
    }
    return app_mesh_persistence_save_outbox(relay, now_ms + 1u);
}

static int original_ack_and_confirm_save(struct mesh_relay *relay,
                                         uint32_t now_ms)
{
    struct proto_packet ack = {0};
    struct mesh_relay_result retry_result;
    struct mesh_relay_result result;
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;
    int ret;

    if (relay->pending.packet.msg_type != MSG_SELF_TEST_REPORT ||
        relay->pending.packet.session_id != TEST_SESSION_ID ||
        relay->pending.packet.seq != TEST_PACKET_SEQ ||
        relay->pending.payload_len != sizeof(self_test_payload) ||
        memcmp(relay->pending.payload,
               self_test_payload,
               sizeof(self_test_payload)) != 0) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_tick(relay,
                          now_ms + RELAY_BUSY_RETRY_MIN_MS,
                          &retry_result);
    if (ret != PROTO_OK ||
        (retry_result.actions & MESH_RELAY_ACTION_RETRANSMIT) == 0u ||
        retry_result.retransmit.next_hop_id != GATEWAY_ID) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    ret = build_gateway_ack(&ack,
                            ack_payload,
                            sizeof(ack_payload),
                            &ack_payload_len,
                            &relay->pending.packet,
                            relay->pending.payload,
                            relay->pending.payload_len,
                            1u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_handle_rx(relay,
                               &ack,
                               ack_payload,
                               ack_payload_len,
                               GATEWAY_ID,
                               90u,
                               now_ms + RELAY_BUSY_RETRY_MIN_MS + 1u,
                               &result);
    if (ret != PROTO_OK ||
        (result.actions &
         MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) == 0u ||
        relay->pending.packet.msg_type != MSG_GATEWAY_ACK_CONFIRM) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    return app_mesh_persistence_save_outbox(
        relay, now_ms + RELAY_BUSY_RETRY_MIN_MS + 2u);
}

static int confirm_ack_and_terminal_clear(struct mesh_relay *relay,
                                          uint32_t now_ms)
{
    struct proto_packet confirm_packet;
    struct proto_packet original_packet;
    struct proto_packet ack = {0};
    struct mesh_relay_result retry_result;
    struct mesh_relay_result result;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t confirm_payload_len;
    size_t ack_payload_len = 0u;
    int ret;

    if (relay->pending.packet.msg_type != MSG_GATEWAY_ACK_CONFIRM ||
        relay->pending.payload_len != sizeof(confirm_payload)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_tick(relay,
                          now_ms + RELAY_BUSY_RETRY_MIN_MS,
                          &retry_result);
    if (ret != PROTO_OK ||
        (retry_result.actions & MESH_RELAY_ACTION_RETRANSMIT) == 0u ||
        retry_result.retransmit.next_hop_id != GATEWAY_ID) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    confirm_packet = relay->pending.packet;
    confirm_payload_len = relay->pending.payload_len;
    memcpy(confirm_payload, relay->pending.payload, confirm_payload_len);
    ret = mesh_gateway_ack_confirm_identity_packet(&confirm_packet,
                                                   confirm_payload,
                                                   confirm_payload_len,
                                                   &original_packet,
                                                   original_digest);
    if (ret != PROTO_OK ||
        original_packet.msg_type != MSG_SELF_TEST_REPORT ||
        original_packet.session_id != TEST_SESSION_ID ||
        original_packet.seq != TEST_PACKET_SEQ) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    ret = build_gateway_ack(&ack,
                            ack_payload,
                            sizeof(ack_payload),
                            &ack_payload_len,
                            &confirm_packet,
                            confirm_payload,
                            confirm_payload_len,
                            2u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_handle_rx(relay,
                               &ack,
                               ack_payload,
                               ack_payload_len,
                               GATEWAY_ID,
                               90u,
                               now_ms + RELAY_BUSY_RETRY_MIN_MS + 1u,
                               &result);
    if (ret != PROTO_OK ||
        (result.actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) == 0u ||
        !mesh_relay_tx_active(relay)) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    ret = app_mesh_persistence_complete_confirmed_producer(
        &original_packet, original_digest);
    if (ret < 0) {
        return ret;
    }
    ret = mesh_relay_commit_gateway_ack_confirm_terminal(relay,
                                                         &confirm_packet,
                                                         confirm_payload,
                                                         confirm_payload_len,
                                                         now_ms +
                                                             RELAY_BUSY_RETRY_MIN_MS +
                                                             2u);
    if (ret != PROTO_OK) {
        return ret;
    }
    return app_mesh_persistence_clear_outbox();
}

int main(void)
{
    struct mesh_relay relay;
    struct route_candidate route;
    uint32_t first_event;
    uint32_t second_event;
    uint32_t boot_index;
    uint32_t now_ms;
    int ret;

    ret = app_click_event_sequence_init();
    if (ret < 0) {
        fail(1, "sequence-init", ret);
        return 1;
    }
    ret = app_click_event_sequence_next(&first_event);
    if (ret < 0) {
        fail(2, "sequence-first", ret);
        return 2;
    }
    ret = app_click_event_sequence_next(&second_event);
    if (ret < 0 || second_event != first_event + 1u ||
        first_event <= APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR) {
        fail(3, "sequence-order", ret);
        return 3;
    }
    boot_index =
        (first_event - APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR - 1u) /
        APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE;
    if (boot_index > 3u) {
        fail(4, "unexpected-boot-index", (int)boot_index);
        return 4;
    }
    now_ms = 1000u + boot_index * 1000u;
    route = direct_gateway_route(now_ms);
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_CLICKER,
                    CLICKER_ID,
                    GATEWAY_ID,
                    1u);
    ret = route_upsert_candidate(&relay.upstream, &route);
    if (ret != PROTO_OK) {
        fail(5, "route", ret);
        return 5;
    }
    ret = app_mesh_persistence_restore_outbox(&relay, now_ms);
    if (ret < 0) {
        fail(6, "restore", ret);
        return 6;
    }

    switch (boot_index) {
    case 0u:
        if (mesh_relay_tx_active(&relay)) {
            fail(7, "first-boot-not-empty", 0);
            return 7;
        }
        ret = original_save(&relay, now_ms);
        break;
    case 1u:
        if (!mesh_relay_tx_active(&relay)) {
            fail(8, "original-missing", 0);
            return 8;
        }
        ret = original_ack_and_confirm_save(&relay, now_ms);
        break;
    case 2u:
        if (!mesh_relay_tx_active(&relay)) {
            fail(9, "confirm-missing", 0);
            return 9;
        }
        ret = confirm_ack_and_terminal_clear(&relay, now_ms);
        break;
    case 3u:
        ret = mesh_relay_tx_active(&relay) ? -EIO : 0;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    if (ret < 0) {
        fail(10, "stage", ret);
        return 10;
    }

    printk("SHARED_NVS boot=%u first=%u second=%u state=%s\n",
           boot_index,
           first_event,
           second_event,
           boot_index == 0u ? "original" :
           boot_index == 1u ? "confirm" :
           boot_index == 2u ? "terminal" : "empty");
    posix_exit(0);
    return 0;
}
