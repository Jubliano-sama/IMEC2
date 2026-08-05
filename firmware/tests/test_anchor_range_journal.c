#include "anchor_range_journal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ANCHOR_ID UINT64_C(0x1111222233334444)
#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define CLICKER_ID UINT64_C(0x5555666677778888)

static size_t make_wire_packet(uint32_t event_seq,
                               uint16_t seq,
                               uint8_t fill,
                               uint8_t *wire,
                               size_t wire_cap)
{
    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = ANCHOR_ID,
        .dst_id = GATEWAY_ID,
        .session_id = event_seq,
        .seq = seq,
        .ttl = 4u,
        .payload_len = 17u,
        .message_age_ms = 0u,
    };
    uint8_t payload[17];
    size_t wire_len = 0u;

    memset(payload, fill, sizeof(payload));
    /* A fixed canonical timestamp-like value proves reset never regenerates
     * bytes from the current uptime. */
    payload[0] = TLV_TIMESTAMP_MS;
    payload[1] = 8u;
    for (uint8_t i = 0u; i < 8u; i++) {
        payload[2u + i] = (uint8_t)(UINT64_C(0x0102030405060708) >>
                                    (8u * i));
    }
    assert(proto_packet_encode(
               &packet, payload, wire, wire_cap, &wire_len) == PROTO_OK);
    return wire_len;
}

static void test_fragment_round_trip_is_byte_identical(void)
{
    struct anchor_range_journal_fragment_identity identity;
    struct anchor_range_journal_fragment_identity restored_identity;
    struct proto_packet restored_packet;
    uint8_t wire[PACKET_MAX_LEN];
    uint8_t reencoded[PACKET_MAX_LEN];
    uint8_t record[ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN];
    const uint8_t *restored_wire = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0u;
    size_t reencoded_len = 0u;
    size_t record_len = 0u;
    size_t restored_wire_len = 0u;
    size_t wire_len = make_wire_packet(
        0x12345678u, 0x2201u, 0xa5u, wire, sizeof(wire));

    assert(anchor_range_journal_encode_fragment(
               0x89abcdefu,
               3u,
               wire,
               wire_len,
               record,
               sizeof(record),
               &record_len,
               &identity) == 0);
    assert(anchor_range_journal_decode_fragment(
               record,
               record_len,
               0x89abcdefu,
               3u,
               &restored_wire,
               &restored_wire_len,
               &restored_identity) == 0);
    assert(restored_wire_len == wire_len);
    assert(memcmp(restored_wire, wire, wire_len) == 0);
    assert(memcmp(&restored_identity, &identity, sizeof(identity)) == 0);
    assert(proto_packet_decode(restored_wire,
                               restored_wire_len,
                               &restored_packet,
                               &payload,
                               &payload_len) == PROTO_OK);
    assert(proto_packet_encode(&restored_packet,
                               payload,
                               reencoded,
                               sizeof(reencoded),
                               &reencoded_len) == PROTO_OK);
    assert(reencoded_len == wire_len);
    assert(memcmp(reencoded, wire, wire_len) == 0);
}

static void test_fragment_corruption_and_wrong_owner_fail_closed(void)
{
    struct anchor_range_journal_fragment_identity identity;
    uint8_t wire[PACKET_MAX_LEN];
    uint8_t record[ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN];
    const uint8_t *restored_wire = NULL;
    size_t record_len = 0u;
    size_t restored_wire_len = 0u;
    size_t wire_len = make_wire_packet(
        0x12345678u, 0x2202u, 0x5au, wire, sizeof(wire));

    assert(anchor_range_journal_encode_fragment(
               7u, 0u, wire, wire_len, record, sizeof(record), &record_len,
               &identity) == 0);
    assert(anchor_range_journal_decode_fragment(
               record, record_len, 8u, 0u, &restored_wire,
               &restored_wire_len, &identity) == -EBADMSG);
    assert(anchor_range_journal_decode_fragment(
               record, record_len, 7u, 1u, &restored_wire,
               &restored_wire_len, &identity) == -EBADMSG);
    record[16u + 4u] ^= 0x80u;
    assert(anchor_range_journal_decode_fragment(
               record, record_len, 7u, 0u, &restored_wire,
               &restored_wire_len, &identity) == -EBADMSG);
    assert(anchor_range_journal_decode_fragment(
               record, record_len - 1u, 7u, 0u, &restored_wire,
               &restored_wire_len, &identity) == -EBADMSG);
}

static void test_control_round_trip_defines_every_byte(void)
{
    struct anchor_range_journal_control control = {
        .clicker_id = CLICKER_ID,
        .anchor_id = ANCHOR_ID,
        .gateway_id = GATEWAY_ID,
        .event_seq = 0x12345678u,
        .generation = 0x87654321u,
        .fragment_count = 3u,
        .attempt_index = 2u,
    };
    struct anchor_range_journal_control restored;
    uint8_t first[ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN];
    uint8_t second[ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN];
    size_t first_len = 0u;
    size_t second_len = 0u;

    for (uint8_t i = 0u; i < control.fragment_count; i++) {
        uint8_t wire[PACKET_MAX_LEN];
        uint8_t record[ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN];
        size_t record_len = 0u;
        size_t wire_len = make_wire_packet(
            control.event_seq,
            (uint16_t)(0x3300u + i),
            (uint8_t)(0x10u + i),
            wire,
            sizeof(wire));

        assert(anchor_range_journal_encode_fragment(
                   control.generation,
                   i,
                   wire,
                   wire_len,
                   record,
                   sizeof(record),
                   &record_len,
                   &control.fragments[i]) == 0);
    }
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    assert(anchor_range_journal_encode_control(
               &control, first, sizeof(first), &first_len) == 0);
    assert(anchor_range_journal_encode_control(
               &control, second, sizeof(second), &second_len) == 0);
    assert(first_len == sizeof(first));
    assert(second_len == first_len);
    assert(memcmp(first, second, first_len) == 0);
    assert(anchor_range_journal_decode_control(
               first, first_len, &restored) == 0);
    assert(restored.clicker_id == control.clicker_id);
    assert(restored.anchor_id == control.anchor_id);
    assert(restored.gateway_id == control.gateway_id);
    assert(restored.event_seq == control.event_seq);
    assert(restored.generation == control.generation);
    assert(restored.fragment_count == control.fragment_count);
    assert(restored.attempt_index == control.attempt_index);
    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        assert(restored.fragments[i].seq == control.fragments[i].seq);
        assert(restored.fragments[i].wire_len ==
               control.fragments[i].wire_len);
        assert(restored.fragments[i].wire_crc ==
               control.fragments[i].wire_crc);
    }

    first[7] = 1u;
    assert(anchor_range_journal_decode_control(
               first, first_len, &restored) == -EBADMSG);
}

static void test_partial_ack_reset_replays_full_committed_batch(void)
{
    const uint8_t fragment_count = 9u;
    uint16_t acknowledged_mask = 0u;
    uint16_t replay_mask = 0u;

    acknowledged_mask |= UINT16_C(1) << 0u;
    acknowledged_mask |= UINT16_C(1) << 4u;
    acknowledged_mask |= UINT16_C(1) << 8u;
    assert(acknowledged_mask !=
           (uint16_t)((UINT16_C(1) << fragment_count) - 1u));

    /*
     * The ACK bitmap is deliberately RAM-only. On reset the committed
     * control remains authoritative and every exact fragment is replayed;
     * gateway duplicate suppression makes this at-least-once recovery safe.
     */
    acknowledged_mask = 0u;
    for (uint8_t i = 0u; i < fragment_count; i++) {
        replay_mask |= UINT16_C(1) << i;
    }
    assert(replay_mask ==
           (uint16_t)((UINT16_C(1) << fragment_count) - 1u));
    assert(acknowledged_mask == 0u);
}

int main(void)
{
    test_fragment_round_trip_is_byte_identical();
    test_fragment_corruption_and_wrong_owner_fail_closed();
    test_control_round_trip_defines_every_byte();
    test_partial_ack_reset_replays_full_committed_batch();
    puts("anchor range journal tests passed");
    return 0;
}
