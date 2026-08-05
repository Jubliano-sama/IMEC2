#ifndef ANCHOR_RANGE_JOURNAL_H
#define ANCHOR_RANGE_JOURNAL_H

#include "protocol.h"
#include "report.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANCHOR_RANGE_JOURNAL_VERSION 1u
#define ANCHOR_RANGE_JOURNAL_FRAGMENT_MAGIC UINT32_C(0x41524631)
#define ANCHOR_RANGE_JOURNAL_CONTROL_MAGIC UINT32_C(0x41524331)
#define ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN 16u
#define ANCHOR_RANGE_JOURNAL_FRAGMENT_CHECKSUM_LEN 2u
#define ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN \
    (ANCHOR_RANGE_JOURNAL_FRAGMENT_PREFIX_LEN + PACKET_MAX_LEN + \
     ANCHOR_RANGE_JOURNAL_FRAGMENT_CHECKSUM_LEN)
#define ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN \
    (40u + RANGE_REPORT_MAX_PACKET_FRAGMENTS * 8u + 2u)

struct anchor_range_journal_fragment_identity {
    uint16_t seq;
    uint16_t wire_len;
    uint16_t wire_crc;
};

struct anchor_range_journal_control {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint64_t gateway_id;
    uint32_t event_seq;
    uint32_t generation;
    struct anchor_range_journal_fragment_identity
        fragments[RANGE_REPORT_MAX_PACKET_FRAGMENTS];
    uint8_t fragment_count;
    uint8_t attempt_index;
};

/*
 * These records are canonical byte streams. No C structure representation,
 * enum width, bool width, or padding byte is ever written to persistence.
 */
int anchor_range_journal_encode_fragment(
    uint32_t generation,
    uint8_t fragment_index,
    const uint8_t *wire_packet,
    size_t wire_packet_len,
    uint8_t *record,
    size_t record_cap,
    size_t *record_len,
    struct anchor_range_journal_fragment_identity *identity);
int anchor_range_journal_decode_fragment(
    const uint8_t *record,
    size_t record_len,
    uint32_t expected_generation,
    uint8_t expected_fragment_index,
    const uint8_t **wire_packet,
    size_t *wire_packet_len,
    struct anchor_range_journal_fragment_identity *identity);
int anchor_range_journal_encode_control(
    const struct anchor_range_journal_control *control,
    uint8_t *record,
    size_t record_cap,
    size_t *record_len);
int anchor_range_journal_decode_control(
    const uint8_t *record,
    size_t record_len,
    struct anchor_range_journal_control *control);

#ifdef __cplusplus
}
#endif

#endif
