#include "serial_frame.h"

#include <assert.h>
#include <string.h>

static void test_serial_frame_round_trip_adds_zero_delimiter(void)
{
    const uint8_t payload[] = {0x01u, 0x00u, 0x02u, 0x03u};
    const struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = 0x1111222233334444ull,
        .dst_id = 0x9999888877776666ull,
        .session_id = 123u,
        .seq = 7u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    struct proto_packet decoded = {0};
    uint8_t decoded_payload[16];
    size_t decoded_payload_len = 0u;

    assert(serial_frame_encode_packet(&packet, payload, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(frame_len > 0u);
    assert(frame[frame_len - 1u] == SERIAL_FRAME_DELIMITER);
    for (size_t i = 0u; i < frame_len - 1u; i++) {
        assert(frame[i] != SERIAL_FRAME_DELIMITER);
    }

    assert(serial_frame_decode_packet(frame,
                                      frame_len,
                                      &decoded,
                                      decoded_payload,
                                      sizeof(decoded_payload),
                                      &decoded_payload_len) == PROTO_OK);
    assert(decoded.msg_type == packet.msg_type);
    assert(decoded.flags == packet.flags);
    assert(decoded.src_id == packet.src_id);
    assert(decoded.dst_id == packet.dst_id);
    assert(decoded.session_id == packet.session_id);
    assert(decoded.seq == packet.seq);
    assert(decoded.ttl == packet.ttl);
    assert(decoded_payload_len == sizeof(payload));
    assert(memcmp(decoded_payload, payload, sizeof(payload)) == 0);
}

static void test_serial_frame_rejects_missing_delimiter(void)
{
    const struct proto_packet packet = {
        .msg_type = MSG_ANCHOR_HEARTBEAT,
        .src_id = 1u,
        .dst_id = 2u,
        .session_id = 3u,
        .seq = 4u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    struct proto_packet decoded = {0};
    uint8_t payload[1];
    size_t payload_len = 0u;

    assert(serial_frame_encode_packet(&packet, NULL, frame, sizeof(frame), &frame_len) == PROTO_OK);
    frame[frame_len - 1u] = 0x01u;
    assert(serial_frame_decode_packet(frame,
                                      frame_len,
                                      &decoded,
                                      payload,
                                      sizeof(payload),
                                      &payload_len) == PROTO_ERR_MALFORMED);
}

static void test_serial_frame_rejects_small_output(void)
{
    const struct proto_packet packet = {
        .msg_type = MSG_ANCHOR_HEARTBEAT,
        .src_id = 1u,
        .dst_id = 2u,
        .session_id = 3u,
        .seq = 4u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    uint8_t frame[4];
    size_t frame_len = 0u;

    assert(serial_frame_encode_packet(&packet, NULL, frame, sizeof(frame), &frame_len) == PROTO_ERR_NO_SPACE);
}

int main(void)
{
    test_serial_frame_round_trip_adds_zero_delimiter();
    test_serial_frame_rejects_missing_delimiter();
    test_serial_frame_rejects_small_output();
    return 0;
}
