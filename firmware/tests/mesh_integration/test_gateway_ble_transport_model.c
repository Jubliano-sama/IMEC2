#include "gateway_ble_transport.h"

#include <inttypes.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK_TRUE(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,            \
                    #expression);                                                \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_INT(actual_expression, expected_expression)                        \
    do {                                                                         \
        int actual_value = (actual_expression);                                  \
        int expected_value = (expected_expression);                              \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%d expected %d\n",                         \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_SIZE(actual_expression, expected_expression)                       \
    do {                                                                         \
        size_t actual_value = (size_t)(actual_expression);                       \
        size_t expected_value = (size_t)(expected_expression);                   \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%zu expected %zu\n",                       \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

struct received_packets {
    struct proto_packet packets[4];
    uint8_t payloads[4][32];
    size_t payload_lens[4];
    size_t count;
};

static int capture_packet(const struct proto_packet *packet,
                          const uint8_t *payload,
                          size_t payload_len,
                          void *ctx)
{
    struct received_packets *received = ctx;

    if (received == NULL || packet == NULL || received->count >= 4u ||
        payload_len > sizeof(received->payloads[0])) {
        return PROTO_ERR_NO_SPACE;
    }
    received->packets[received->count] = *packet;
    received->payload_lens[received->count] = payload_len;
    if (payload_len > 0u) {
        memcpy(received->payloads[received->count], payload, payload_len);
    }
    received->count++;
    return PROTO_OK;
}

static struct proto_packet packet(uint8_t msg_type,
                                  uint16_t seq,
                                  size_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = msg_type,
        .flags = msg_type == MSG_CLICK_REPORT ? FLAG_COUNT_AS_CLICK : 0u,
        .src_id = UINT64_C(0x1111222233334444) + seq,
        .dst_id = UINT64_C(0x9999888877776666),
        .session_id = 0x12340000u + seq,
        .seq = seq,
        .ttl = 4u,
        .payload_len = (uint16_t)payload_len,
    };
}

static void test_negotiated_att_limits(void)
{
    CHECK_SIZE(gateway_ble_att_payload_max(0u), 20u);
    CHECK_SIZE(gateway_ble_att_payload_max(23u), 20u);
    CHECK_SIZE(gateway_ble_att_payload_max(247u), 244u);
    CHECK_SIZE(gateway_ble_att_payload_max(517u), 514u);
    CHECK_SIZE(gateway_ble_att_payload_max(600u), 514u);
    CHECK_TRUE(gateway_ble_notification_value_fits(23u, 20u));
    CHECK_TRUE(!gateway_ble_notification_value_fits(23u, 21u));
    CHECK_TRUE(gateway_ble_notification_value_fits(247u, 244u));
    CHECK_TRUE(!gateway_ble_notification_value_fits(247u, 247u));
}

static void test_serial_stream_arbitrary_boundaries(void)
{
    const uint8_t payload_a[] = {0x01u, 0x00u, 0x02u, 0x03u};
    const uint8_t payload_b[] = {0x10u, 0x20u, 0x00u, 0x30u, 0x40u};
    const uint8_t payload_c[] = {0xAAu, 0xBBu};
    struct proto_packet packets[] = {
        packet(MSG_COMMAND, 1u, sizeof(payload_a)),
        packet(MSG_CLICK_REPORT, 2u, sizeof(payload_b)),
        packet(MSG_ANCHOR_HEARTBEAT, 3u, sizeof(payload_c)),
    };
    const uint8_t *payloads[] = {payload_a, payload_b, payload_c};
    uint8_t frames[3][SERIAL_FRAME_MAX_LEN];
    size_t frame_lens[3] = {0u};
    uint8_t coalesced[(2u * SERIAL_FRAME_MAX_LEN) + 3u];
    size_t coalesced_len = 0u;
    struct gateway_ble_rx_stream stream;
    struct received_packets received = {0};
    size_t decoded = 0u;

    for (size_t i = 0u; i < 3u; i++) {
        CHECK_INT(serial_frame_encode_packet(&packets[i],
                                             payloads[i],
                                             frames[i],
                                             sizeof(frames[i]),
                                             &frame_lens[i]),
                  PROTO_OK);
    }

    gateway_ble_rx_stream_init(&stream);
    CHECK_INT(gateway_ble_rx_stream_feed(&stream,
                                         frames[0],
                                         1u,
                                         capture_packet,
                                         &received,
                                         &decoded),
              PROTO_OK);
    CHECK_SIZE(decoded, 0u);

    memcpy(&coalesced[coalesced_len], &frames[0][1], frame_lens[0] - 1u);
    coalesced_len += frame_lens[0] - 1u;
    memcpy(&coalesced[coalesced_len], frames[1], frame_lens[1]);
    coalesced_len += frame_lens[1];
    memcpy(&coalesced[coalesced_len], frames[2], 3u);
    coalesced_len += 3u;
    CHECK_INT(gateway_ble_rx_stream_feed(&stream,
                                         coalesced,
                                         coalesced_len,
                                         capture_packet,
                                         &received,
                                         &decoded),
              PROTO_OK);
    CHECK_SIZE(decoded, 2u);
    CHECK_SIZE(received.count, 2u);
    CHECK_SIZE(stream.frame_len, 3u);

    CHECK_INT(gateway_ble_rx_stream_feed(&stream,
                                         &frames[2][3],
                                         frame_lens[2] - 3u,
                                         capture_packet,
                                         &received,
                                         &decoded),
              PROTO_OK);
    CHECK_SIZE(decoded, 1u);
    CHECK_SIZE(received.count, 3u);
    CHECK_SIZE(stream.decoded_frames, 3u);
    CHECK_SIZE(stream.rejected_frames, 0u);
    for (size_t i = 0u; i < 3u; i++) {
        CHECK_INT(received.packets[i].msg_type, packets[i].msg_type);
        CHECK_INT(received.packets[i].seq, packets[i].seq);
        CHECK_SIZE(received.payload_lens[i], packets[i].payload_len);
        CHECK_TRUE(memcmp(received.payloads[i],
                          payloads[i],
                          received.payload_lens[i]) == 0);
    }
}

static void test_tx_cursor_retry_and_mtu_change(void)
{
    uint8_t frame[300];
    struct gateway_ble_tx_cursor cursor;
    const uint8_t *first_chunk;
    const uint8_t *chunk;
    size_t first_len;
    size_t chunk_len;

    for (size_t i = 0u; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)i;
    }
    gateway_ble_tx_cursor_init(&cursor, frame, sizeof(frame), 23u);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &first_chunk, &first_len),
              PROTO_OK);
    CHECK_SIZE(first_len, 20u);
    CHECK_INT(gateway_ble_tx_cursor_set_mtu(&cursor, 247u), PROTO_ERR_BUSY);
    CHECK_INT(gateway_ble_tx_cursor_complete(&cursor, false), PROTO_OK);
    CHECK_SIZE(cursor.offset, 0u);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &chunk, &chunk_len),
              PROTO_OK);
    CHECK_TRUE(chunk == first_chunk);
    CHECK_SIZE(chunk_len, first_len);
    CHECK_INT(gateway_ble_tx_cursor_complete(&cursor, true), PROTO_OK);
    CHECK_SIZE(cursor.offset, 20u);
    CHECK_INT(gateway_ble_tx_cursor_set_mtu(&cursor, 247u), PROTO_OK);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &chunk, &chunk_len),
              PROTO_OK);
    CHECK_SIZE(chunk_len, 244u);
    CHECK_INT(gateway_ble_tx_cursor_complete(&cursor, true), PROTO_OK);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &chunk, &chunk_len),
              PROTO_OK);
    CHECK_SIZE(chunk_len, 36u);
    CHECK_INT(gateway_ble_tx_cursor_complete(&cursor, true), PROTO_OK);
    CHECK_TRUE(gateway_ble_tx_cursor_done(&cursor));
}

static void test_connection_credit_backpressure_and_recovery(void)
{
    struct gateway_ble_link link;
    uint8_t completed = 0u;

    gateway_ble_link_init(&link, 30000u, 2u);
    CHECK_INT(gateway_ble_link_connect(&link, 0u, 247u, true),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 244u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 244u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 1u),
              GATEWAY_BLE_LINK_ERR_NO_CREDIT);
    CHECK_INT(gateway_ble_link_run_connection_event(&link, 29999u, &completed),
              GATEWAY_BLE_LINK_ERR_EVENT_TIME);
    CHECK_INT(gateway_ble_link_run_connection_event(&link, 30000u, &completed),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(completed, 2);
    CHECK_INT(link.available_credits, 2);

    gateway_ble_link_set_stalled(&link, true);
    CHECK_INT(gateway_ble_link_try_notify(&link, 244u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 244u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_run_connection_event(&link, 60000u, &completed),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(completed, 0);
    CHECK_INT(link.available_credits, 0);
    CHECK_INT(gateway_ble_link_try_notify(&link, 1u),
              GATEWAY_BLE_LINK_ERR_NO_CREDIT);
    gateway_ble_link_set_stalled(&link, false);
    CHECK_INT(gateway_ble_link_run_connection_event(&link, 90000u, &completed),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(completed, 2);
    CHECK_INT(link.available_credits, 2);

    CHECK_INT(gateway_ble_link_set_mtu(&link, 23u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 21u),
              GATEWAY_BLE_LINK_ERR_VALUE_TOO_LONG);
    CHECK_INT(gateway_ble_link_try_notify(&link, 20u), GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_disconnect(&link), 1);
    CHECK_INT(gateway_ble_link_try_notify(&link, 20u),
              GATEWAY_BLE_LINK_ERR_NOT_CONNECTED);
    CHECK_INT(gateway_ble_link_connect(&link, 100000u, 23u, false),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_try_notify(&link, 20u),
              GATEWAY_BLE_LINK_ERR_NOTIFY_DISABLED);
    CHECK_INT(gateway_ble_link_disconnect(&link), 0);
    CHECK_INT(link.notifications_dropped_disconnect, 1);
}

static void test_disconnect_retries_same_cursor_chunk(void)
{
    uint8_t frame[260] = {0};
    struct gateway_ble_tx_cursor cursor;
    struct gateway_ble_link link;
    const uint8_t *first_chunk;
    const uint8_t *retry_chunk;
    size_t first_len;
    size_t retry_len;

    gateway_ble_tx_cursor_init(&cursor, frame, sizeof(frame), 247u);
    gateway_ble_link_init(&link, 30000u, 1u);
    CHECK_INT(gateway_ble_link_connect(&link, 0u, 247u, true),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &first_chunk, &first_len),
              PROTO_OK);
    CHECK_SIZE(first_len, 244u);
    CHECK_INT(gateway_ble_link_try_notify(&link, first_len),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_link_disconnect(&link), 1);
    CHECK_INT(gateway_ble_tx_cursor_complete(&cursor, false), PROTO_OK);
    CHECK_SIZE(cursor.offset, 0u);

    CHECK_INT(gateway_ble_link_connect(&link, 50000u, 247u, true),
              GATEWAY_BLE_LINK_OK);
    CHECK_INT(gateway_ble_tx_cursor_begin(&cursor, &retry_chunk, &retry_len),
              PROTO_OK);
    CHECK_TRUE(retry_chunk == first_chunk);
    CHECK_SIZE(retry_len, first_len);
}

static size_t ingress_frame(uint8_t type, uint16_t seq, uint8_t *frame)
{
    const uint8_t payload[] = {1u, 2u, 3u};
    struct proto_packet value = packet(type, seq, sizeof(payload));
    size_t len = 0u;

    CHECK_INT(serial_frame_encode_packet(&value, payload, frame,
                                         SERIAL_FRAME_MAX_LEN, &len), PROTO_OK);
    return len;
}

static void test_receipt_reservation_and_command_fifo(void)
{
    struct gateway_ble_ingress ingress = {0};
    struct gateway_ble_ingress_frame selected;
    uint8_t frames[4][SERIAL_FRAME_MAX_LEN];
    size_t lens[4];
    uint8_t receipts = 0u;

    for (uint16_t i = 0u; i < 4u; i++) {
        lens[i] = ingress_frame(MSG_COMMAND, i + 1u, frames[i]);
    }
    for (size_t i = 0u; i < 3u; i++) {
        CHECK_INT(gateway_ble_ingress_write(&ingress, frames[i], lens[i],
                                            &receipts), 0);
        CHECK_INT(receipts, 0);
    }
    CHECK_INT(gateway_ble_ingress_write(&ingress, frames[3], lens[3],
                                        &receipts), -ENOSPC);
    lens[3] = ingress_frame(MSG_GATEWAY_HOST_RECEIPT, 44u, frames[3]);
    CHECK_INT(gateway_ble_ingress_write(&ingress, frames[3], lens[3],
                                        &receipts), 0);
    CHECK_INT(receipts, 1);
    CHECK_INT(ingress.count, 4);
    CHECK_INT(gateway_ble_ingress_take(&ingress, true, &selected), 0);
    CHECK_SIZE(selected.len, lens[3]);
    CHECK_TRUE(memcmp(selected.frame, frames[3], lens[3]) == 0);
    CHECK_INT(gateway_ble_ingress_take(&ingress, true, &selected), -ENOENT);
    /* A producer arriving after receipt extraction still cannot displace
     * the reserved slot, and queued command-to-command order is unchanged. */
    CHECK_INT(gateway_ble_ingress_write(&ingress, frames[0], lens[0], NULL),
              -ENOSPC);
    for (size_t i = 0u; i < 3u; i++) {
        CHECK_INT(gateway_ble_ingress_take(&ingress, false, &selected), 0);
        CHECK_SIZE(selected.len, lens[i]);
        CHECK_TRUE(memcmp(selected.frame, frames[i], lens[i]) == 0);
    }
    CHECK_INT(ingress.count, 0);
}

static void test_ingress_write_failure_is_atomic_at_every_chunk_boundary(void)
{
    uint8_t command[SERIAL_FRAME_MAX_LEN];
    uint8_t receipt[SERIAL_FRAME_MAX_LEN];
    uint8_t write[2u * SERIAL_FRAME_MAX_LEN];
    size_t command_len = ingress_frame(MSG_COMMAND, 1u, command);
    size_t receipt_len = ingress_frame(MSG_GATEWAY_HOST_RECEIPT, 2u, receipt);

    for (size_t split = 0u; split < receipt_len; split++) {
        struct gateway_ble_ingress ingress = {0};
        struct gateway_ble_ingress saved;
        struct gateway_ble_ingress_frame selected;
        uint8_t receipts = 9u;
        size_t write_len = receipt_len - split;

        for (size_t i = 0u; i < 3u; i++) {
            CHECK_INT(gateway_ble_ingress_write(&ingress, command,
                                                command_len, NULL), 0);
        }
        CHECK_INT(gateway_ble_ingress_write(&ingress, receipt, split, NULL), 0);
        saved = ingress;
        memcpy(write, &receipt[split], write_len);
        memcpy(&write[write_len], command, command_len);
        write_len += command_len;
        /* The receipt prefix must not be admitted if the next frame in the
         * same ATT write cannot fit. Retrying the identical write is safe. */
        CHECK_INT(gateway_ble_ingress_write(&ingress, write, write_len,
                                            &receipts), -ENOSPC);
        CHECK_INT(receipts, 0);
        CHECK_TRUE(memcmp(&saved, &ingress, sizeof(saved)) == 0);
        CHECK_INT(gateway_ble_ingress_take(&ingress, false, &selected), 0);
        CHECK_INT(gateway_ble_ingress_write(&ingress, write, write_len,
                                            &receipts), 0);
        CHECK_INT(receipts, 1);
        CHECK_INT(ingress.count, 4);
        CHECK_INT(gateway_ble_ingress_take(&ingress, true, &selected), 0);
        CHECK_SIZE(selected.len, receipt_len);
        CHECK_TRUE(memcmp(selected.frame, receipt, receipt_len) == 0);
    }
}

static void test_oversized_ingress_does_not_pin_next_frame(void)
{
    struct gateway_ble_ingress ingress = {0};
    struct gateway_ble_ingress_frame selected;
    uint8_t oversized[SERIAL_FRAME_MAX_LEN + 1u];
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t len = ingress_frame(MSG_COMMAND, 5u, frame);

    CHECK_INT(gateway_ble_ingress_write(&ingress, frame, len, NULL), 0);
    memset(oversized, 1, sizeof(oversized));
    CHECK_INT(gateway_ble_ingress_write(&ingress, oversized,
                                        sizeof(oversized) - 1u, NULL), 0);
    CHECK_INT(gateway_ble_ingress_write(&ingress, &oversized[0], 1u, NULL),
              -EMSGSIZE);
    CHECK_INT(ingress.partial_len, 0);
    CHECK_INT(ingress.count, 1);
    CHECK_INT(gateway_ble_ingress_write(&ingress, frame, len, NULL), 0);
    CHECK_INT(gateway_ble_ingress_take(&ingress, false, &selected), 0);
    CHECK_TRUE(memcmp(selected.frame, frame, len) == 0);
    CHECK_INT(gateway_ble_ingress_take(&ingress, false, &selected), 0);
    CHECK_TRUE(memcmp(selected.frame, frame, len) == 0);
}

static void test_ingress_reconnect_preserves_completed_frames(void)
{
    struct gateway_ble_ingress ingress = {0};
    struct gateway_ble_ingress_frame selected;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t len = ingress_frame(MSG_GATEWAY_HOST_RECEIPT, 5u, frame);

    CHECK_INT(gateway_ble_ingress_write(&ingress, frame, len, NULL), 0);
    CHECK_INT(gateway_ble_ingress_write(&ingress, frame, len / 2u, NULL), 0);
    gateway_ble_ingress_reset_partial(&ingress);
    CHECK_INT(ingress.partial_len, 0);
    CHECK_INT(ingress.count, 1);
    CHECK_INT(gateway_ble_ingress_take(&ingress, true, &selected), 0);
    CHECK_SIZE(selected.len, len);
    CHECK_TRUE(memcmp(selected.frame, frame, len) == 0);
}

int main(void)
{
    test_receipt_reservation_and_command_fifo();
    test_ingress_write_failure_is_atomic_at_every_chunk_boundary();
    test_ingress_reconnect_preserves_completed_frames();
    test_oversized_ingress_does_not_pin_next_frame();
    test_negotiated_att_limits();
    test_serial_stream_arbitrary_boundaries();
    test_tx_cursor_retry_and_mtu_change();
    test_connection_credit_backpressure_and_recovery();
    test_disconnect_retries_same_cursor_chunk();

    if (failures != 0u) {
        fprintf(stderr, "gateway BLE transport model tests failed: %u\n",
                failures);
        return 1;
    }
    puts("gateway BLE transport model tests passed");
    return 0;
}
