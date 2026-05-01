#include "mesh_ble.h"

#include "mesh.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0x1111222233334444ull
#define ANCHOR_B 0x5555666677778888ull
#define GATEWAY  0x9999888877776666ull

static struct mesh_outbound hop_ack_outbound(void)
{
    struct mesh_outbound out = {
        .next_hop_id = ANCHOR_A,
    };
    size_t payload_len = 0u;

    assert(mesh_append_requested_seq(out.payload, sizeof(out.payload), &payload_len, 17u) == PROTO_OK);
    assert(mesh_init_hop_ack(&out.packet,
                             ANCHOR_B,
                             ANCHOR_A,
                             123u,
                             9u,
                             (uint8_t)payload_len) == PROTO_OK);
    out.payload_len = (uint8_t)payload_len;
    return out;
}

static void test_frame_round_trip_keeps_next_hop_outside_inner_packet(void)
{
    struct mesh_outbound out = hop_ack_outbound();
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint64_t previous_hop_id = 0u;
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    assert(mesh_ble_frame_encode(ANCHOR_B, &out, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(frame_len > MESH_BLE_FRAME_HEADER_LEN);

    assert(mesh_ble_frame_decode(frame,
                                 frame_len,
                                 ANCHOR_A,
                                 &previous_hop_id,
                                 &packet,
                                 payload,
                                 sizeof(payload),
                                 &payload_len) == PROTO_OK);

    assert(previous_hop_id == ANCHOR_B);
    assert(packet.msg_type == MSG_MESH_ACK);
    assert(packet.src_id == ANCHOR_B);
    assert(packet.dst_id == ANCHOR_A);
    assert(packet.session_id == out.packet.session_id);
    assert(packet.seq == out.packet.seq);
    assert(payload_len == out.payload_len);
    assert(memcmp(payload, out.payload, payload_len) == 0);
}

static void test_decode_rejects_wrong_next_hop_and_self_echo(void)
{
    struct mesh_outbound out = hop_ack_outbound();
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint64_t previous_hop_id = 0u;
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    assert(mesh_ble_frame_encode(ANCHOR_B, &out, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(mesh_ble_frame_decode(frame,
                                 frame_len,
                                 GATEWAY,
                                 &previous_hop_id,
                                 &packet,
                                 payload,
                                 sizeof(payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);

    proto_put_u64_le(&frame[2], ANCHOR_A);
    assert(mesh_ble_frame_decode(frame,
                                 frame_len,
                                 ANCHOR_A,
                                 &previous_hop_id,
                                 &packet,
                                 payload,
                                 sizeof(payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);
}

static void test_decode_accepts_broadcast_next_hop(void)
{
    struct mesh_outbound out = hop_ack_outbound();
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint64_t previous_hop_id = 0u;
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    out.next_hop_id = MESH_BROADCAST_ID;
    out.packet.msg_type = MSG_ROUTE_ADV;
    out.packet.dst_id = MESH_BROADCAST_ID;
    assert(mesh_ble_frame_encode(GATEWAY, &out, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(mesh_ble_frame_decode(frame,
                                 frame_len,
                                 ANCHOR_A,
                                 &previous_hop_id,
                                 &packet,
                                 payload,
                                 sizeof(payload),
                                 &payload_len) == PROTO_OK);
    assert(previous_hop_id == GATEWAY);
    assert(packet.dst_id == MESH_BROADCAST_ID);
}

static void test_encode_rejects_payloads_that_do_not_fit_one_ad_structure(void)
{
    struct mesh_outbound out = {
        .next_hop_id = ANCHOR_B,
        .packet = {
            .msg_type = MSG_COMMAND,
            .flags = FLAG_ACK_REQUESTED,
            .src_id = GATEWAY,
            .dst_id = ANCHOR_A,
            .session_id = 55u,
            .seq = 2u,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = MESH_BLE_MAX_PAYLOAD_LEN + 1u,
        },
        .payload_len = MESH_BLE_MAX_PAYLOAD_LEN + 1u,
    };
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    assert(mesh_ble_frame_encode(GATEWAY, &out, frame, sizeof(frame), &frame_len) == PROTO_ERR_ARG);
}

int main(void)
{
    test_frame_round_trip_keeps_next_hop_outside_inner_packet();
    test_decode_rejects_wrong_next_hop_and_self_echo();
    test_decode_accepts_broadcast_next_hop();
    test_encode_rejects_payloads_that_do_not_fit_one_ad_structure();
    return 0;
}
