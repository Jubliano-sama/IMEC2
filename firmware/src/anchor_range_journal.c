#include "anchor_range_journal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

static void put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
}

static void put_u32_le(uint8_t *out, uint32_t value)
{
    for (uint8_t i = 0u; i < 4u; i++) {
        out[i] = (uint8_t)(value >> (8u * i));
    }
}

static void put_u64_le(uint8_t *out, uint64_t value)
{
    for (uint8_t i = 0u; i < 8u; i++) {
        out[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t get_u16_le(const uint8_t *in)
{
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8u);
}

static uint32_t get_u32_le(const uint8_t *in)
{
    uint32_t value = 0u;

    for (uint8_t i = 0u; i < 4u; i++) {
        value |= (uint32_t)in[i] << (8u * i);
    }
    return value;
}

static uint64_t get_u64_le(const uint8_t *in)
{
    uint64_t value = 0u;

    for (uint8_t i = 0u; i < 8u; i++) {
        value |= (uint64_t)in[i] << (8u * i);
    }
    return value;
}

static bool fragment_identity_valid(
    const struct anchor_range_journal_fragment_identity *identity)
{
    return identity != NULL &&
           identity->wire_len >= PACKET_HEADER_LEN + PACKET_CRC_LEN &&
           identity->wire_len <= PACKET_MAX_LEN;
}

int anchor_range_journal_encode_fragment(
    uint32_t generation,
    uint8_t fragment_index,
    const uint8_t *wire_packet,
    size_t wire_packet_len,
    uint8_t *record,
    size_t record_cap,
    size_t *record_len,
    struct anchor_range_journal_fragment_identity *identity)
{
    size_t required_len;
    uint16_t wire_crc;
    uint16_t record_crc;

    if (generation == 0u ||
        fragment_index >= RANGE_REPORT_MAX_PACKET_FRAGMENTS ||
        wire_packet == NULL ||
        wire_packet_len < PACKET_HEADER_LEN + PACKET_CRC_LEN ||
        wire_packet_len > PACKET_MAX_LEN ||
        record == NULL || record_len == NULL || identity == NULL) {
        return -EINVAL;
    }
    required_len = ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN +
                   wire_packet_len +
                   ANCHOR_RANGE_JOURNAL_FRAGMENT_CHECKSUM_LEN;
    if (record_cap < required_len) {
        return -ENOSPC;
    }

    memset(record, 0, required_len);
    wire_crc = proto_crc16_ccitt_false(wire_packet, wire_packet_len);
    put_u32_le(&record[0], ANCHOR_RANGE_JOURNAL_FRAGMENT_MAGIC);
    record[4] = ANCHOR_RANGE_JOURNAL_VERSION;
    record[5] = fragment_index;
    put_u32_le(&record[8], generation);
    put_u16_le(&record[12], (uint16_t)wire_packet_len);
    put_u16_le(&record[14], wire_crc);
    memcpy(&record[ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN],
           wire_packet,
           wire_packet_len);
    record_crc = proto_crc16_ccitt_false(record, required_len - 2u);
    put_u16_le(&record[required_len - 2u], record_crc);

    identity->seq = get_u16_le(&wire_packet[24]);
    identity->wire_len = (uint16_t)wire_packet_len;
    identity->wire_crc = wire_crc;
    *record_len = required_len;
    return 0;
}

int anchor_range_journal_decode_fragment(
    const uint8_t *record,
    size_t record_len,
    uint32_t expected_generation,
    uint8_t expected_fragment_index,
    const uint8_t **wire_packet,
    size_t *wire_packet_len,
    struct anchor_range_journal_fragment_identity *identity)
{
    size_t declared_len;
    const uint8_t *wire;
    uint16_t declared_wire_crc;

    if (record == NULL || wire_packet == NULL || wire_packet_len == NULL ||
        identity == NULL || expected_generation == 0u ||
        expected_fragment_index >= RANGE_REPORT_MAX_PACKET_FRAGMENTS ||
        record_len < ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN +
                     ANCHOR_RANGE_JOURNAL_FRAGMENT_CHECKSUM_LEN) {
        return -EINVAL;
    }
    declared_len = get_u16_le(&record[12]);
    if (get_u32_le(&record[0]) != ANCHOR_RANGE_JOURNAL_FRAGMENT_MAGIC ||
        record[4] != ANCHOR_RANGE_JOURNAL_VERSION ||
        record[5] != expected_fragment_index ||
        record[6] != 0u || record[7] != 0u ||
        get_u32_le(&record[8]) != expected_generation ||
        declared_len < PACKET_HEADER_LEN + PACKET_CRC_LEN ||
        declared_len > PACKET_MAX_LEN ||
        record_len != ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN +
                      declared_len +
                      ANCHOR_RANGE_JOURNAL_FRAGMENT_CHECKSUM_LEN ||
        get_u16_le(&record[record_len - 2u]) !=
            proto_crc16_ccitt_false(record, record_len - 2u)) {
        return -EBADMSG;
    }
    wire = &record[ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN];
    declared_wire_crc = get_u16_le(&record[14]);
    if (declared_wire_crc != proto_crc16_ccitt_false(wire, declared_len)) {
        return -EBADMSG;
    }

    identity->seq = get_u16_le(&wire[24]);
    identity->wire_len = (uint16_t)declared_len;
    identity->wire_crc = declared_wire_crc;
    *wire_packet = wire;
    *wire_packet_len = declared_len;
    return 0;
}

static bool control_valid(const struct anchor_range_journal_control *control)
{
    if (control == NULL || control->generation == 0u ||
        control->clicker_id == 0u || control->event_seq == 0u ||
        control->anchor_id == 0u || control->gateway_id == 0u ||
        control->fragment_count == 0u ||
        control->fragment_count > RANGE_REPORT_MAX_PACKET_FRAGMENTS) {
        return false;
    }
    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        const struct anchor_range_journal_fragment_identity *identity =
            &control->fragments[i];

        if (i < control->fragment_count) {
            if (!fragment_identity_valid(identity)) {
                return false;
            }
        } else if (identity->seq != 0u || identity->wire_len != 0u ||
                   identity->wire_crc != 0u) {
            return false;
        }
    }
    return true;
}

int anchor_range_journal_encode_control(
    const struct anchor_range_journal_control *control,
    uint8_t *record,
    size_t record_cap,
    size_t *record_len)
{
    size_t offset = 40u;

    if (!control_valid(control) || record == NULL || record_len == NULL) {
        return -EINVAL;
    }
    if (record_cap < ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN) {
        return -ENOSPC;
    }

    memset(record, 0, ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN);
    put_u32_le(&record[0], ANCHOR_RANGE_JOURNAL_CONTROL_MAGIC);
    record[4] = ANCHOR_RANGE_JOURNAL_VERSION;
    record[5] = control->fragment_count;
    record[6] = control->attempt_index;
    put_u32_le(&record[8], control->generation);
    put_u64_le(&record[12], control->clicker_id);
    put_u32_le(&record[20], control->event_seq);
    put_u64_le(&record[24], control->anchor_id);
    put_u64_le(&record[32], control->gateway_id);
    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        put_u16_le(&record[offset], control->fragments[i].seq);
        put_u16_le(&record[offset + 2u], control->fragments[i].wire_len);
        put_u16_le(&record[offset + 4u], control->fragments[i].wire_crc);
        offset += 8u;
    }
    put_u16_le(&record[ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN - 2u],
               proto_crc16_ccitt_false(
                   record, ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN - 2u));
    *record_len = ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN;
    return 0;
}

int anchor_range_journal_decode_control(
    const uint8_t *record,
    size_t record_len,
    struct anchor_range_journal_control *control)
{
    size_t offset = 40u;

    if (record == NULL || control == NULL) {
        return -EINVAL;
    }
    memset(control, 0, sizeof(*control));
    if (record_len != ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN ||
        get_u32_le(&record[0]) != ANCHOR_RANGE_JOURNAL_CONTROL_MAGIC ||
        record[4] != ANCHOR_RANGE_JOURNAL_VERSION ||
        record[7] != 0u ||
        get_u16_le(&record[record_len - 2u]) !=
            proto_crc16_ccitt_false(record, record_len - 2u)) {
        return -EBADMSG;
    }

    control->fragment_count = record[5];
    control->attempt_index = record[6];
    control->generation = get_u32_le(&record[8]);
    control->clicker_id = get_u64_le(&record[12]);
    control->event_seq = get_u32_le(&record[20]);
    control->anchor_id = get_u64_le(&record[24]);
    control->gateway_id = get_u64_le(&record[32]);
    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        control->fragments[i].seq = get_u16_le(&record[offset]);
        control->fragments[i].wire_len = get_u16_le(&record[offset + 2u]);
        control->fragments[i].wire_crc = get_u16_le(&record[offset + 4u]);
        if (record[offset + 6u] != 0u || record[offset + 7u] != 0u) {
            memset(control, 0, sizeof(*control));
            return -EBADMSG;
        }
        offset += 8u;
    }
    if (!control_valid(control)) {
        memset(control, 0, sizeof(*control));
        return -EBADMSG;
    }
    return 0;
}
