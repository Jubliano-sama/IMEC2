#include "uwb.h"

#include <assert.h>
#include <string.h>

static struct uwb_range_header header(uint8_t type, uint8_t flags)
{
    const struct uwb_range_header value = {
        .type = type,
        .seq = 17u,
        .round_index = 3u,
        .network_id = 0x494D4543u,
        .session_id = 0xAABBCCDDu,
        .session_nonce = UINT64_C(0x0102030405060708),
        .initiator_short_addr = 0x7788u,
        .responder_short_addr = 0x2211u,
        .flags = flags,
        .initiator_id = UINT64_C(0x1111222233337788),
        .responder_id = UINT64_C(0xAAAA555500002211),
    };
    return value;
}

static void assert_same_header(const struct uwb_range_header *actual,
                               const struct uwb_range_header *expected)
{
    assert(actual->type == expected->type);
    assert(actual->seq == expected->seq);
    assert(actual->round_index == expected->round_index);
    assert(actual->network_id == expected->network_id);
    assert(actual->session_id == expected->session_id);
    assert(actual->session_nonce == expected->session_nonce);
    assert(actual->initiator_short_addr == expected->initiator_short_addr);
    assert(actual->responder_short_addr == expected->responder_short_addr);
    assert(actual->flags == expected->flags);
    assert(actual->initiator_id == expected->initiator_id);
    assert(actual->responder_id == expected->responder_id);
}

static void test_poll_round_trip_diagnostic_not_click(void)
{
    const struct uwb_range_header poll = header(MSG_UWB_POLL, FLAG_DIAGNOSTIC);
    struct uwb_range_header decoded = {0};
    uint8_t buf[UWB_POLL_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_POLL_LEN);
    assert(buf[0] == UWB_MARKER);
    assert(buf[1] == UWB_VERSION);
    assert(buf[2] == MSG_UWB_POLL);
    assert(UWB_POLL_LEN == 42u);
    assert(UWB_RESP_LEN == 50u);
    assert(UWB_FINAL_LEN == 54u);
    assert(UWB_REPORT_LEN == 50u);

    assert(uwb_decode_poll(buf, written, &decoded) == PROTO_OK);
    assert_same_header(&decoded, &poll);
    assert((decoded.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((decoded.flags & FLAG_COUNT_AS_CLICK) == 0u);
}

static void test_response_final_and_report_round_trip(void)
{
    const struct uwb_response_frame response = {
        .header = header(MSG_UWB_RESP, FLAG_COUNT_AS_CLICK),
        .poll_rx_ts_32 = 0x01020304u,
        .resp_tx_ts_32 = 0x05060708u,
    };
    const struct uwb_final_frame final = {
        .header = header(MSG_UWB_FINAL, FLAG_COUNT_AS_CLICK),
        .poll_tx_ts_32 = 0x11111111u,
        .resp_rx_ts_32 = 0x22222222u,
        .final_tx_ts_32 = 0x33333333u,
    };
    const struct uwb_report_frame report = {
        .header = header(MSG_UWB_REPORT, FLAG_COUNT_AS_CLICK),
        .distance_mm = -1234,
        .quality = 91u,
        .status = RANGE_OK,
        .rsl_dbm = -72,
    };
    struct uwb_response_frame decoded_response = {0};
    struct uwb_final_frame decoded_final = {0};
    struct uwb_report_frame decoded_report = {0};
    uint8_t buf[UWB_FINAL_LEN];
    size_t written = 0u;

    assert(uwb_encode_response(&response, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_RESP_LEN);
    assert(uwb_decode_response(buf, written, &decoded_response) == PROTO_OK);
    assert_same_header(&decoded_response.header, &response.header);
    assert(decoded_response.poll_rx_ts_32 == response.poll_rx_ts_32);
    assert(decoded_response.resp_tx_ts_32 == response.resp_tx_ts_32);

    assert(uwb_encode_final(&final, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_FINAL_LEN);
    assert(uwb_decode_final(buf, written, &decoded_final) == PROTO_OK);
    assert_same_header(&decoded_final.header, &final.header);
    assert(decoded_final.poll_tx_ts_32 == final.poll_tx_ts_32);
    assert(decoded_final.resp_rx_ts_32 == final.resp_rx_ts_32);
    assert(decoded_final.final_tx_ts_32 == final.final_tx_ts_32);

    assert(uwb_encode_report(&report, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_REPORT_LEN);
    assert(uwb_decode_report(buf, written, &decoded_report) == PROTO_OK);
    assert_same_header(&decoded_report.header, &report.header);
    assert(decoded_report.distance_mm == report.distance_mm);
    assert(decoded_report.quality == report.quality);
    assert(decoded_report.status == report.status);
    assert(decoded_report.rsl_dbm == report.rsl_dbm);
}

static void test_clicker_diag_round_trip(void)
{
    const uint8_t diag_bytes[] = {0xA0u, 0xA1u, 0xA2u};
    const struct uwb_clicker_diag_frame diag = {
        .header = header(MSG_UWB_CLICKER_DIAG, FLAG_COUNT_AS_CLICK),
        .final_tx_ts_32 = 0x11223344u,
        .status_flags = 0x00000005u,
        .irq_latency_us = 37u,
        .resp_quality = 93u,
        .resp_rsl_dbm = -68,
        .diag_len = sizeof(diag_bytes),
        .diag_bytes = {0xA0u, 0xA1u, 0xA2u},
    };
    struct uwb_clicker_diag_frame decoded = {0};
    uint8_t buf[UWB_CLICKER_DIAG_MAX_LEN];
    size_t written = 0u;

    assert(sizeof(diag_bytes) == diag.diag_len);
    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_CLICKER_DIAG_FIXED_LEN + diag.diag_len);
    assert(UWB_CLICKER_DIAG_FIXED_LEN == 57u);
    assert(buf[0] == UWB_MARKER);
    assert(buf[1] == UWB_VERSION);
    assert(buf[2] == MSG_UWB_CLICKER_DIAG);

    assert(uwb_decode_clicker_diag(buf, written, &decoded) == PROTO_OK);
    assert_same_header(&decoded.header, &diag.header);
    assert(decoded.final_tx_ts_32 == diag.final_tx_ts_32);
    assert(decoded.status_flags == diag.status_flags);
    assert(decoded.irq_latency_us == diag.irq_latency_us);
    assert(decoded.resp_quality == diag.resp_quality);
    assert(decoded.resp_rsl_dbm == diag.resp_rsl_dbm);
    assert(decoded.diag_len == diag.diag_len);
    assert(memcmp(decoded.diag_bytes, diag_bytes, sizeof(diag_bytes)) == 0);
}

static void test_rejects_bad_clicker_diag(void)
{
    struct uwb_clicker_diag_frame diag = {
        .header = header(MSG_UWB_CLICKER_DIAG, FLAG_COUNT_AS_CLICK),
        .final_tx_ts_32 = 0x11223344u,
        .status_flags = 0x00000001u,
        .irq_latency_us = 37u,
        .resp_quality = 93u,
        .resp_rsl_dbm = -68,
        .diag_len = 1u,
        .diag_bytes = {0xA0u},
    };
    uint8_t buf[UWB_CLICKER_DIAG_MAX_LEN];
    size_t written = 0u;

    diag.status_flags = 0u;
    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) ==
           PROTO_ERR_MALFORMED);

    diag.status_flags = 0x00000001u;
    diag.resp_quality = 101u;
    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) ==
           PROTO_ERR_MALFORMED);

    diag.resp_quality = 93u;
    diag.diag_len = UWB_CLICKER_DIAG_MAX_BYTES + 1u;
    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) ==
           PROTO_ERR_MALFORMED);

    diag.diag_len = 1u;
    assert(uwb_encode_clicker_diag(&diag,
                                   buf,
                                   UWB_CLICKER_DIAG_FIXED_LEN,
                                   &written) == PROTO_ERR_NO_SPACE);

    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) == PROTO_OK);
    buf[UWB_HEADER_LEN + 12u] = 101u;
    assert(uwb_decode_clicker_diag(buf,
                                   written,
                                   &(struct uwb_clicker_diag_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_clicker_diag(&diag, buf, sizeof(buf), &written) == PROTO_OK);
    assert(uwb_decode_clicker_diag(buf,
                                   written - 1u,
                                   &(struct uwb_clicker_diag_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_rejects_invalid_header_and_status(void)
{
    struct uwb_range_header bad_header = header(MSG_UWB_POLL, FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK);
    const struct uwb_report_frame report = {
        .header = header(MSG_UWB_REPORT, FLAG_DIAGNOSTIC),
        .distance_mm = 0,
        .quality = 80u,
        .status = RANGE_OK,
        .rsl_dbm = -70,
    };
    struct uwb_report_frame bad_report = {
        .header = header(MSG_UWB_REPORT, FLAG_DIAGNOSTIC),
        .distance_mm = 0,
        .quality = 101u,
        .status = RANGE_OK,
    };
    uint8_t buf[UWB_REPORT_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, 0u);
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK | FLAG_GATEWAY_ACK_REQUIRED);
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.network_id = 0u;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.seq = 0u;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.session_nonce = 0u;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.initiator_id = 0u;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.responder_id = bad_header.initiator_id;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.initiator_short_addr++;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_header = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    bad_header.responder_short_addr++;
    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_report(&bad_report, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    bad_report.quality = 80u;
    bad_report.status = RANGE_STS_QUALITY_FAIL;
    assert(uwb_encode_report(&bad_report, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);

    assert(uwb_encode_report(&report, buf, sizeof(buf), &written) == PROTO_OK);
    buf[UWB_HEADER_LEN + 4u] = 101u;
    assert(uwb_decode_report(buf, written, &(struct uwb_report_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_report(&report, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[UWB_HEADER_LEN + 5u], (uint16_t)RANGE_STS_QUALITY_FAIL);
    assert(uwb_decode_report(buf, written, &(struct uwb_report_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_decode_rejects_wrong_type(void)
{
    const struct uwb_range_header poll = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    uint8_t buf[UWB_POLL_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    buf[2] = MSG_UWB_RESP;
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) == PROTO_ERR_MALFORMED);
}

static void test_decode_rejects_mismatched_full_id_short_address(void)
{
    const struct uwb_range_header poll = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    uint8_t buf[UWB_POLL_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) == PROTO_OK);

    buf[20] ^= 0x01u;
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    buf[33] ^= 0x01u;
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_decode_rejects_malformed_range_header_fields(void)
{
    const struct uwb_range_header poll = header(MSG_UWB_POLL, FLAG_COUNT_AS_CLICK);
    uint8_t buf[UWB_POLL_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    buf[3] = 0u;
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u32_le(&buf[4], 0u);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u32_le(&buf[8], 0u);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u64_le(&buf[12], 0u);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[22], poll.initiator_short_addr);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    buf[24] = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK;
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_poll(&poll, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u64_le(&buf[33], poll.initiator_id);
    assert(uwb_decode_poll(buf, written, &(struct uwb_range_header){0}) ==
           PROTO_ERR_MALFORMED);
}

static struct uwb_wake_claim_frame wake_claim(void)
{
    const struct uwb_wake_claim_frame claim = {
        .network_id = 0x12345678u,
        .clicker_id = UINT64_C(0x0102030405060708),
        .click_event_id = 77u,
        .attempt_index = 2u,
        .priority_id = UINT64_C(0x1111222233334444),
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 120u,
        .discovery_starts_in_ms = 150u,
        .claimed_duration_ms = 400u,
        .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS,
        .nonce = UINT64_C(0x8877665544332211),
        .flags = FLAG_COUNT_AS_CLICK,
    };
    return claim;
}

static void test_wake_discovery_and_schedule_round_trip(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    const struct uwb_discover_frame discover = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT,
        .flags = FLAG_DIAGNOSTIC,
    };
    const struct uwb_discovery_reply_frame reply = {
        .network_id = claim.network_id,
        .anchor_id = UINT64_C(0xAA00000000000001),
        .selected_clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_slot = 7u,
        .status = UWB_DISCOVERY_REPLY_PRESENT,
        .rx_quality = 88u,
        .battery_mv = 3010u,
        .flags = claim.flags,
    };
    const struct uwb_range_release_frame release = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovered_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u,
        .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .reason = UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
        .flags = claim.flags,
    };
    struct uwb_range_schedule_frame schedule = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .selected_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .ranging_channel = claim.ranging_channel,
        .reply_delay_us = 900u,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS * 2u,
        .min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 2u,
        .flags = claim.flags,
        .entries = {
            {
                .anchor_id = UINT64_C(0xAA00000000000001),
                .seq = 10u,
                .sample_count = 2u,
            },
            {
                .anchor_id = UINT64_C(0xAA00000000000002),
                .seq = 20u,
                .sample_count = 2u,
            },
            {
                .anchor_id = UINT64_C(0xAA00000000000003),
                .seq = 30u,
                .sample_count = 2u,
            },
        },
    };
    uint8_t buf[UWB_RANGE_SCHEDULE_MAX_LEN];
    struct uwb_wake_claim_frame decoded_claim = {0};
    struct uwb_discover_frame decoded_discover = {0};
    struct uwb_discovery_reply_frame decoded_reply = {0};
    struct uwb_range_release_frame decoded_release = {0};
    struct uwb_range_schedule_frame decoded_schedule = {0};
    size_t written = 0u;

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_WAKE_CLAIM_LEN);
    assert(uwb_decode_wake_claim(buf, written, &decoded_claim) == PROTO_OK);
    assert(decoded_claim.network_id == claim.network_id);
    assert(decoded_claim.clicker_id == claim.clicker_id);
    assert(decoded_claim.click_event_id == claim.click_event_id);
    assert(decoded_claim.attempt_index == claim.attempt_index);
    assert(decoded_claim.priority_id == claim.priority_id);
    assert(decoded_claim.wake_channel == claim.wake_channel);
    assert(decoded_claim.ranging_channel == claim.ranging_channel);
    assert(decoded_claim.wake_train_ends_in_ms == claim.wake_train_ends_in_ms);
    assert(decoded_claim.discovery_starts_in_ms == claim.discovery_starts_in_ms);
    assert(decoded_claim.claimed_duration_ms == claim.claimed_duration_ms);
    assert(decoded_claim.min_anchor_count == claim.min_anchor_count);
    assert(decoded_claim.max_anchor_count == claim.max_anchor_count);
    assert(decoded_claim.nonce == claim.nonce);
    assert(decoded_claim.flags == claim.flags);
    buf[10] ^= 0x40u;
    assert(uwb_decode_wake_claim(buf, written, &decoded_claim) == PROTO_ERR_BAD_CRC);
    assert(uwb_encode_wake_claim(&(struct uwb_wake_claim_frame){
                                      .network_id = claim.network_id,
                                      .clicker_id = claim.clicker_id,
                                      .click_event_id = claim.click_event_id,
                                      .attempt_index = claim.attempt_index,
                                      .priority_id = claim.priority_id,
                                      .wake_channel = claim.wake_channel,
                                      .ranging_channel = claim.ranging_channel,
                                      .wake_train_ends_in_ms = claim.wake_train_ends_in_ms,
                                      .discovery_starts_in_ms = claim.discovery_starts_in_ms,
                                      .claimed_duration_ms = claim.claimed_duration_ms,
                                      .min_anchor_count = claim.min_anchor_count,
                                      .max_anchor_count = claim.max_anchor_count,
                                      .nonce = claim.nonce,
                                      .flags = 0u,
                                  },
                                  buf,
                                  sizeof(buf),
                                  &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_wake_claim(&(struct uwb_wake_claim_frame){
                                      .network_id = claim.network_id,
                                      .clicker_id = claim.clicker_id,
                                      .click_event_id = claim.click_event_id,
                                      .attempt_index = claim.attempt_index,
                                      .priority_id = claim.priority_id,
                                      .wake_channel = claim.wake_channel,
                                      .ranging_channel = claim.ranging_channel,
                                      .wake_train_ends_in_ms = claim.wake_train_ends_in_ms,
                                      .discovery_starts_in_ms = claim.discovery_starts_in_ms,
                                      .claimed_duration_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS + 1u,
                                      .min_anchor_count = claim.min_anchor_count,
                                      .max_anchor_count = claim.max_anchor_count,
                                      .nonce = claim.nonce,
                                      .flags = claim.flags,
                                  },
                                  buf,
                                  sizeof(buf),
                                  &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_wake_claim(&(struct uwb_wake_claim_frame){
                                      .network_id = claim.network_id,
                                      .clicker_id = claim.clicker_id,
                                      .click_event_id = claim.click_event_id,
                                      .attempt_index = claim.attempt_index,
                                      .priority_id = claim.priority_id,
                                      .wake_channel = claim.wake_channel,
                                      .ranging_channel = claim.ranging_channel,
                                      .wake_train_ends_in_ms = UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS + 1u,
                                      .discovery_starts_in_ms = UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS + 1u,
                                      .claimed_duration_ms = UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS + 1u,
                                      .min_anchor_count = claim.min_anchor_count,
                                      .max_anchor_count = claim.max_anchor_count,
                                      .nonce = claim.nonce,
                                      .flags = claim.flags,
                                  },
                                  buf,
                                  sizeof(buf),
                                  &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_wake_claim(&(struct uwb_wake_claim_frame){
                                      .network_id = claim.network_id,
                                      .clicker_id = claim.clicker_id,
                                      .click_event_id = claim.click_event_id,
                                      .attempt_index = claim.attempt_index,
                                      .priority_id = claim.priority_id,
                                      .wake_channel = claim.wake_channel,
                                      .ranging_channel = claim.ranging_channel,
                                      .wake_train_ends_in_ms = claim.wake_train_ends_in_ms,
                                      .discovery_starts_in_ms = claim.wake_train_ends_in_ms - 1u,
                                      .claimed_duration_ms = claim.claimed_duration_ms,
                                      .min_anchor_count = claim.min_anchor_count,
                                      .max_anchor_count = claim.max_anchor_count,
                                      .nonce = claim.nonce,
                                      .flags = claim.flags,
                                  },
                                  buf,
                                  sizeof(buf),
                                  &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_wake_claim(&(struct uwb_wake_claim_frame){
                                      .network_id = claim.network_id,
                                      .clicker_id = claim.clicker_id,
                                      .click_event_id = claim.click_event_id,
                                      .attempt_index = claim.attempt_index,
                                      .priority_id = claim.priority_id,
                                      .wake_channel = claim.wake_channel,
                                      .ranging_channel = claim.ranging_channel,
                                      .wake_train_ends_in_ms = claim.wake_train_ends_in_ms,
                                      .discovery_starts_in_ms = claim.discovery_starts_in_ms,
                                      .claimed_duration_ms = claim.discovery_starts_in_ms - 1u,
                                      .min_anchor_count = claim.min_anchor_count,
                                      .max_anchor_count = claim.max_anchor_count,
                                      .nonce = claim.nonce,
                                      .flags = claim.flags,
                                  },
                                  buf,
                                  sizeof(buf),
                                  &written) == PROTO_ERR_MALFORMED);

    assert(uwb_encode_discover(&discover, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_DISCOVER_LEN);
    assert(uwb_decode_discover(buf, written, &decoded_discover) == PROTO_OK);
    assert(decoded_discover.network_id == discover.network_id);
    assert(decoded_discover.clicker_id == discover.clicker_id);
    assert(decoded_discover.click_event_id == discover.click_event_id);
    assert(decoded_discover.attempt_index == discover.attempt_index);
    assert(decoded_discover.nonce == discover.nonce);
    assert(decoded_discover.discovery_slot_count == discover.discovery_slot_count);
    assert(decoded_discover.flags == discover.flags);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_DISCOVERY_REPLY_LEN);
    assert(uwb_decode_discovery_reply(buf, written, &decoded_reply) == PROTO_OK);
    assert(decoded_reply.network_id == reply.network_id);
    assert(decoded_reply.anchor_id == reply.anchor_id);
    assert(decoded_reply.selected_clicker_id == reply.selected_clicker_id);
    assert(decoded_reply.click_event_id == reply.click_event_id);
    assert(decoded_reply.attempt_index == reply.attempt_index);
    assert(decoded_reply.nonce == reply.nonce);
    assert(decoded_reply.anchor_slot == reply.anchor_slot);
    assert(decoded_reply.status == reply.status);
    assert(decoded_reply.rx_quality == reply.rx_quality);
    assert(decoded_reply.battery_mv == reply.battery_mv);
    assert(decoded_reply.flags == reply.flags);

    assert(uwb_encode_range_release(&release, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == UWB_RANGE_RELEASE_LEN);
    assert(UWB_RANGE_RELEASE_LEN == 34u);
    assert(buf[0] == UWB_MARKER);
    assert(buf[1] == UWB_VERSION);
    assert(buf[2] == MSG_UWB_RANGE_RELEASE);
    assert(uwb_decode_range_release(buf, written, &decoded_release) == PROTO_OK);
    assert(decoded_release.network_id == release.network_id);
    assert(decoded_release.clicker_id == release.clicker_id);
    assert(decoded_release.click_event_id == release.click_event_id);
    assert(decoded_release.attempt_index == release.attempt_index);
    assert(decoded_release.nonce == release.nonce);
    assert(decoded_release.discovered_anchor_count == release.discovered_anchor_count);
    assert(decoded_release.min_anchor_count == release.min_anchor_count);
    assert(decoded_release.reason == release.reason);
    assert(decoded_release.flags == release.flags);

    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == uwb_range_schedule_encoded_len(schedule.selected_count));
    assert(uwb_decode_range_schedule(buf, written, &decoded_schedule) == PROTO_OK);
    assert(decoded_schedule.network_id == schedule.network_id);
    assert(decoded_schedule.clicker_id == schedule.clicker_id);
    assert(decoded_schedule.selected_count == schedule.selected_count);
    assert(decoded_schedule.burst_window_ms == schedule.burst_window_ms);
    assert(decoded_schedule.exchange_stride_us == schedule.exchange_stride_us);
    assert(decoded_schedule.max_exchanges == schedule.max_exchanges);
    assert(decoded_schedule.sts_mode == UWB_RANGE_SCHEDULE_STS_DISABLED);
    assert(decoded_schedule.diagnostics_required == UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED);
    assert(decoded_schedule.entries[0].anchor_id == schedule.entries[0].anchor_id);
    assert(decoded_schedule.entries[1].seq == schedule.entries[1].seq);
    assert(decoded_schedule.entries[2].anchor_id == schedule.entries[2].anchor_id);
}

static void test_control_frames_reject_bad_crc(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    const struct uwb_discover_frame discover = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT,
        .flags = claim.flags,
    };
    const struct uwb_discovery_reply_frame reply = {
        .network_id = claim.network_id,
        .anchor_id = UINT64_C(0xAA00000000000001),
        .selected_clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_slot = 7u,
        .status = UWB_DISCOVERY_REPLY_PRESENT,
        .rx_quality = 88u,
        .battery_mv = 3010u,
        .flags = claim.flags,
    };
    const struct uwb_range_release_frame release = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovered_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u,
        .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .reason = UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
        .flags = claim.flags,
    };
    const struct uwb_range_schedule_frame schedule = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .selected_count = 1u,
        .ranging_channel = claim.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = 1u,
        .min_successful_unique_anchors = 1u,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 1u,
        .flags = FLAG_DIAGNOSTIC,
        .entries = {
            {
                .anchor_id = UINT64_C(0xAA00000000000001),
                .seq = 10u,
                .sample_count = 1u,
            },
        },
    };
    uint8_t buf[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t written = 0u;

    assert(uwb_encode_discover(&discover, buf, sizeof(buf), &written) == PROTO_OK);
    buf[3] ^= 0x01u;
    assert(uwb_decode_discover(buf, written, &(struct uwb_discover_frame){0}) ==
           PROTO_ERR_BAD_CRC);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    buf[11] ^= 0x01u;
    assert(uwb_decode_discovery_reply(buf,
                                      written,
                                      &(struct uwb_discovery_reply_frame){0}) ==
           PROTO_ERR_BAD_CRC);

    assert(uwb_encode_range_release(&release, buf, sizeof(buf), &written) == PROTO_OK);
    buf[20] ^= 0x01u;
    assert(uwb_decode_range_release(buf,
                                    written,
                                    &(struct uwb_range_release_frame){0}) ==
           PROTO_ERR_BAD_CRC);

    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_OK);
    buf[32] ^= 0x01u;
    assert(uwb_decode_range_schedule(buf,
                                     written,
                                     &(struct uwb_range_schedule_frame){0}) ==
           PROTO_ERR_BAD_CRC);
}

static void refresh_frame_crc(uint8_t *frame, size_t len)
{
    assert(frame != NULL);
    assert(len >= UWB_FRAME_CRC_LEN);
    proto_put_u16_le(&frame[len - UWB_FRAME_CRC_LEN],
                     proto_crc16_ccitt_false(frame, len - UWB_FRAME_CRC_LEN));
}

static void refresh_uwb_mesh_inner_and_outer_crc(uint8_t *frame, size_t len)
{
    uint16_t packet_len;
    uint8_t *packet;

    assert(frame != NULL);
    assert(len >= UWB_MESH_FRAME_HEADER_LEN + PACKET_HEADER_LEN + PACKET_CRC_LEN +
                      UWB_FRAME_CRC_LEN);
    packet_len = proto_get_u16_le(&frame[23]);
    assert(packet_len >= PACKET_HEADER_LEN + PACKET_CRC_LEN);
    assert((size_t)packet_len == len - UWB_MESH_FRAME_HEADER_LEN - UWB_FRAME_CRC_LEN);

    packet = &frame[UWB_MESH_FRAME_HEADER_LEN];
    proto_put_u16_le(&packet[packet_len - PACKET_CRC_LEN],
                     proto_crc16_ccitt_false(packet, packet_len - PACKET_CRC_LEN));
    refresh_frame_crc(frame, len);
}

static void test_wake_claim_decode_rejects_valid_crc_malformed_timing(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    uint8_t buf[UWB_WAKE_CLAIM_LEN];
    size_t written = 0u;

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[30], 0u);
    refresh_frame_crc(buf, written);
    assert(uwb_decode_wake_claim(buf,
                                 written,
                                 &(struct uwb_wake_claim_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[32], claim.wake_train_ends_in_ms - 1u);
    refresh_frame_crc(buf, written);
    assert(uwb_decode_wake_claim(buf,
                                 written,
                                 &(struct uwb_wake_claim_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[34], claim.discovery_starts_in_ms - 1u);
    refresh_frame_crc(buf, written);
    assert(uwb_decode_wake_claim(buf,
                                 written,
                                 &(struct uwb_wake_claim_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[32], UWB_WAKE_CLAIM_MAX_DISCOVERY_START_MS + 1u);
    proto_put_u16_le(&buf[34], UWB_WAKE_CLAIM_MAX_DISCOVERY_START_MS + 1u);
    refresh_frame_crc(buf, written);
    assert(uwb_decode_wake_claim(buf,
                                 written,
                                 &(struct uwb_wake_claim_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_wake_claim(&claim, buf, sizeof(buf), &written) == PROTO_OK);
    proto_put_u16_le(&buf[34], UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS + 1u);
    refresh_frame_crc(buf, written);
    assert(uwb_decode_wake_claim(buf,
                                 written,
                                 &(struct uwb_wake_claim_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_discovery_decode_rejects_valid_crc_malformed_fields(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    const struct uwb_discover_frame discover = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT,
        .flags = FLAG_DIAGNOSTIC,
    };
    const struct uwb_discovery_reply_frame reply = {
        .network_id = claim.network_id,
        .anchor_id = UINT64_C(0xAA00000000000001),
        .selected_clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_slot = 7u,
        .status = UWB_DISCOVERY_REPLY_PRESENT,
        .rx_quality = 88u,
        .battery_mv = 3010u,
        .flags = FLAG_DIAGNOSTIC,
    };
    uint8_t buf[UWB_DISCOVERY_REPLY_LEN];
    size_t written = 0u;

    assert(uwb_encode_discover(&discover, buf, sizeof(buf), &written) == PROTO_OK);
    buf[28] = 0u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discover(buf, written, &(struct uwb_discover_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discover(&discover, buf, sizeof(buf), &written) == PROTO_OK);
    buf[28] = UWB_DISCOVERY_SLOT_COUNT + 1u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discover(buf, written, &(struct uwb_discover_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discover(&discover, buf, sizeof(buf), &written) == PROTO_OK);
    buf[29] = 0u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discover(buf, written, &(struct uwb_discover_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    buf[36] = UWB_DISCOVERY_SLOT_COUNT;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discovery_reply(buf,
                                      written,
                                      &(struct uwb_discovery_reply_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    buf[37] = 0xffu;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discovery_reply(buf,
                                      written,
                                      &(struct uwb_discovery_reply_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    buf[38] = 101u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discovery_reply(buf,
                                      written,
                                      &(struct uwb_discovery_reply_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_discovery_reply(&reply, buf, sizeof(buf), &written) == PROTO_OK);
    buf[41] = 0u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_discovery_reply(buf,
                                      written,
                                      &(struct uwb_discovery_reply_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_range_release_decode_rejects_valid_crc_malformed_fields(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    const struct uwb_range_release_frame release = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovered_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u,
        .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .reason = UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
        .flags = FLAG_DIAGNOSTIC,
    };
    uint8_t buf[UWB_RANGE_RELEASE_LEN];
    size_t written = 0u;

    assert(uwb_encode_range_release(&release, buf, sizeof(buf), &written) == PROTO_OK);
    buf[28] = release.min_anchor_count;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_range_release(buf,
                                    written,
                                    &(struct uwb_range_release_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_range_release(&release, buf, sizeof(buf), &written) == PROTO_OK);
    buf[30] = 0xffu;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_range_release(buf,
                                    written,
                                    &(struct uwb_range_release_frame){0}) ==
           PROTO_ERR_MALFORMED);

    assert(uwb_encode_range_release(&release, buf, sizeof(buf), &written) == PROTO_OK);
    buf[31] = 0u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_range_release(buf,
                                    written,
                                    &(struct uwb_range_release_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_range_schedule_decode_rejects_valid_crc_malformed_fields(void)
{
    const struct uwb_wake_claim_frame claim = wake_claim();
    const struct uwb_range_schedule_frame schedule = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .selected_count = 1u,
        .ranging_channel = claim.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = 1u,
        .min_successful_unique_anchors = 1u,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 1u,
        .flags = FLAG_DIAGNOSTIC,
        .entries = {
            {
                .anchor_id = UINT64_C(0xAA00000000000001),
                .seq = 10u,
                .sample_count = 1u,
            },
        },
    };
    uint8_t buf[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t written = 0u;

    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_OK);
    buf[28] = 2u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_range_schedule(buf,
                                     written,
                                     &(struct uwb_range_schedule_frame){0}) ==
           PROTO_ERR_BAD_LENGTH);

    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_OK);
    buf[UWB_RANGE_SCHEDULE_FIXED_LEN + 9u] = schedule.samples_per_anchor + 1u;
    refresh_frame_crc(buf, written);
    assert(uwb_decode_range_schedule(buf,
                                     written,
                                     &(struct uwb_range_schedule_frame){0}) ==
           PROTO_ERR_MALFORMED);
}

static void test_schedule_rejects_unsafe_ranging_params(void)
{
    struct uwb_range_schedule_frame schedule = {
        .network_id = 1u,
        .clicker_id = 2u,
        .click_event_id = 3u,
        .attempt_index = 1u,
        .nonce = 4u,
        .selected_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .reply_delay_us = 900u,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 1u,
        .flags = FLAG_COUNT_AS_CLICK,
        .entries = {
            {
                .anchor_id = 10u,
                .seq = 1u,
                .sample_count = 1u,
            },
            {
                .anchor_id = 10u,
                .seq = 2u,
                .sample_count = 1u,
            },
            {
                .anchor_id = 12u,
                .seq = 3u,
                .sample_count = 1u,
            },
        },
    };
    uint8_t buf[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t written = 0u;

    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.entries[1].anchor_id = 11u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS - 1u;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US + 1u;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.flags = 0u;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.flags = FLAG_COUNT_AS_CLICK | FLAG_GATEWAY_ACK_REQUIRED;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.flags = FLAG_COUNT_AS_CLICK;
    schedule.samples_per_anchor = 2u;
    schedule.entries[0].sample_count = 1u;
    schedule.entries[1].sample_count = 2u;
    schedule.entries[2].sample_count = 2u;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    schedule.samples_per_anchor = 2u;
    schedule.entries[0].sample_count = 2u;
    schedule.entries[1].sample_count = 2u;
    schedule.entries[2].sample_count = 2u;
    schedule.max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS * 2u;
    schedule.entries[1].seq = 255u;
    assert(uwb_encode_range_schedule(&schedule, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
}

static void test_normal_click_schedule_requires_three_selected_anchors(void)
{
    struct uwb_range_schedule_frame schedule = {
        .network_id = 1u,
        .clicker_id = 2u,
        .click_event_id = 3u,
        .attempt_index = 1u,
        .nonce = 4u,
        .selected_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .reply_delay_us = UWB_RANGE_REPLY_DELAY_UUS,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 1u,
        .flags = FLAG_COUNT_AS_CLICK,
        .entries = {
            {.anchor_id = 10u, .seq = 1u, .sample_count = 1u},
            {.anchor_id = 11u, .seq = 2u, .sample_count = 1u},
            {.anchor_id = 12u, .seq = 3u, .sample_count = 1u},
        },
    };

    assert(uwb_validate_range_schedule(&schedule) == PROTO_OK);

    schedule.selected_count = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u;
    schedule.max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u;
    schedule.min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u;
    assert(uwb_validate_range_schedule(&schedule) == PROTO_ERR_MALFORMED);

    schedule.selected_count = UWB_NORMAL_CLICK_MIN_ANCHORS;
    schedule.max_exchanges = UWB_NORMAL_CLICK_MIN_ANCHORS;
    schedule.min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS - 1u;
    assert(uwb_validate_range_schedule(&schedule) == PROTO_ERR_MALFORMED);

    schedule.selected_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS + 1u;
    schedule.min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS;
    assert(uwb_validate_range_schedule(&schedule) == PROTO_ERR_MALFORMED);
}

static void test_schedule_samples_are_round_robin(void)
{
    const struct uwb_range_schedule_frame schedule = {
        .network_id = 1u,
        .clicker_id = 2u,
        .click_event_id = 3u,
        .attempt_index = 1u,
        .nonce = 4u,
        .selected_count = 4u,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .reply_delay_us = 900u,
        .first_poll_delay_ms = 3u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .burst_window_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .max_exchanges = 8u,
        .min_successful_unique_anchors = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED,
        .diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED,
        .samples_per_anchor = 2u,
        .flags = FLAG_COUNT_AS_CLICK,
        .entries = {
            {.anchor_id = 1u, .seq = 10u, .sample_count = 2u},
            {.anchor_id = 2u, .seq = 20u, .sample_count = 2u},
            {.anchor_id = 3u, .seq = 30u, .sample_count = 2u},
            {.anchor_id = 4u, .seq = 40u, .sample_count = 2u},
        },
    };
    const uint64_t expected_anchors[] = {1u, 2u, 3u, 4u, 1u, 2u, 3u, 4u};
    const uint8_t expected_seq[] = {10u, 20u, 30u, 40u, 11u, 21u, 31u, 41u};

    assert(uwb_range_schedule_total_samples(&schedule) == 8u);
    for (size_t i = 0u; i < 8u; i++) {
        uint64_t anchor_id = 0u;
        uint8_t seq = 0u;

        assert(uwb_range_schedule_sample_at(&schedule, i, &anchor_id, &seq) == PROTO_OK);
        assert(anchor_id == expected_anchors[i]);
        assert(seq == expected_seq[i]);
    }
    assert(uwb_range_schedule_sample_at(&schedule, 8u, &(uint64_t){0}, &(uint8_t){0}) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_anchor_epoch_arbitrates_and_locks(void)
{
    struct uwb_anchor_epoch epoch = {0};
    struct uwb_wake_claim_frame first = wake_claim();
    struct uwb_wake_claim_frame hidden = wake_claim();
    struct uwb_wake_claim_frame retry = wake_claim();
    enum uwb_anchor_claim_decision decision;

    hidden.clicker_id = UINT64_C(0x0102030405060709);
    hidden.click_event_id = 78u;
    hidden.priority_id = first.priority_id + 1u;
    hidden.nonce = first.nonce + 1u;

    retry.clicker_id = UINT64_C(0x010203040506070a);
    retry.click_event_id = 79u;
    retry.attempt_index = (uint8_t)(first.attempt_index + 1u);
    retry.priority_id = first.priority_id + 100u;
    retry.nonce = first.nonce + 2u;

    assert(uwb_anchor_epoch_consider_claim(&epoch, &first, 1000u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(uwb_anchor_epoch_matches(&epoch,
                                    first.network_id,
                                    first.clicker_id,
                                    first.click_event_id,
                                    first.attempt_index,
                                    first.nonce));

    assert(uwb_anchor_epoch_consider_claim(&epoch, &hidden, 1010u, &decision) == PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION);
    assert(epoch.clicker_id == first.clicker_id);

    hidden.priority_id = first.priority_id - 1u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &hidden, 1020u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY);
    assert(epoch.clicker_id == hidden.clicker_id);

    assert(uwb_anchor_epoch_consider_claim(&epoch, &retry, 1030u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY);
    assert(epoch.clicker_id == retry.clicker_id);
    assert(epoch.attempt_index == retry.attempt_index);

    hidden.attempt_index = (uint8_t)(retry.attempt_index - 1u);
    hidden.priority_id = retry.priority_id - 1000u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &hidden, 1040u, &decision) == PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION);
    assert(epoch.clicker_id == retry.clicker_id);

    assert(uwb_anchor_epoch_consider_claim(&epoch, &first, 2000u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(epoch.clicker_id == first.clicker_id);
}

static void test_anchor_epoch_refreshes_repeated_same_attempt_claim(void)
{
    struct uwb_anchor_epoch epoch = {0};
    struct uwb_wake_claim_frame first = wake_claim();
    struct uwb_wake_claim_frame repeated = first;
    enum uwb_anchor_claim_decision decision;

    first.wake_train_ends_in_ms = 430u;
    first.discovery_starts_in_ms = 430u;
    first.claimed_duration_ms = 1365u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &first, 1000u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(epoch.epoch_ends_at_ms == 2365u);

    repeated.wake_train_ends_in_ms = 200u;
    repeated.discovery_starts_in_ms = 200u;
    repeated.claimed_duration_ms = 1135u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &repeated, 1230u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(epoch.clicker_id == first.clicker_id);
    assert(epoch.click_event_id == first.click_event_id);
    assert(epoch.attempt_index == first.attempt_index);
    assert(epoch.priority_id == first.priority_id);
    assert(epoch.nonce == first.nonce);
    assert(epoch.epoch_ends_at_ms == 2365u);
}

static void test_anchor_epoch_rejects_same_event_wrong_session_identity(void)
{
    struct uwb_anchor_epoch epoch = {0};
    struct uwb_wake_claim_frame first = wake_claim();
    struct uwb_wake_claim_frame spoof = first;
    enum uwb_anchor_claim_decision decision;

    assert(uwb_anchor_epoch_consider_claim(&epoch, &first, 1000u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);

    spoof.priority_id = first.priority_id - 1u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &spoof, 1005u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(epoch.clicker_id == first.clicker_id);
    assert(epoch.click_event_id == first.click_event_id);
    assert(epoch.attempt_index == first.attempt_index);
    assert(epoch.priority_id == first.priority_id);
    assert(epoch.nonce == first.nonce);
    assert(epoch.flags == first.flags);

    spoof = first;
    spoof.attempt_index = (uint8_t)(first.attempt_index + 1u);
    spoof.priority_id = first.priority_id - 1u;
    spoof.nonce = first.nonce + 1u;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &spoof, 1010u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(epoch.clicker_id == first.clicker_id);
    assert(epoch.click_event_id == first.click_event_id);
    assert(epoch.attempt_index == first.attempt_index);
    assert(epoch.priority_id == first.priority_id);
    assert(epoch.nonce == first.nonce);
    assert(epoch.flags == first.flags);

    spoof = first;
    spoof.attempt_index = (uint8_t)(first.attempt_index + 1u);
    spoof.priority_id = first.priority_id - 1u;
    spoof.flags = FLAG_DIAGNOSTIC;
    assert(uwb_anchor_epoch_consider_claim(&epoch, &spoof, 1020u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(epoch.attempt_index == first.attempt_index);
    assert(epoch.priority_id == first.priority_id);
    assert(epoch.flags == first.flags);
}

static void test_claim_precedence_compare_uses_canonical_tuple(void)
{
    assert(uwb_claim_precedence_compare(2u, 100u, 50u, 10u,
                                        1u, 1u, 1u, 1u) > 0);
    assert(uwb_claim_precedence_compare(1u, 1u, 1u, 1u,
                                        2u, 100u, 50u, 10u) < 0);
    assert(uwb_claim_precedence_compare(1u, 10u, 50u, 10u,
                                        1u, 11u, 1u, 1u) > 0);
    assert(uwb_claim_precedence_compare(1u, 10u, 40u, 10u,
                                        1u, 10u, 50u, 1u) > 0);
    assert(uwb_claim_precedence_compare(1u, 10u, 50u, 10u,
                                        1u, 10u, 50u, 11u) > 0);
    assert(uwb_claim_precedence_compare(1u, 10u, 50u, 10u,
                                        1u, 10u, 50u, 10u) == 0);
}

static void test_uwb_mesh_frame_round_trip_and_filters(void)
{
    const uint8_t payload[] = {0x01u, 0x02u, 0x03u};
    const struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = 0x10u,
        .dst_id = 0x20u,
        .session_id = 0x30u,
        .seq = 0x40u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };
    const struct proto_packet broadcast_packet = {
        .msg_type = MSG_ROUTE_REQ,
        .flags = 0u,
        .src_id = 0x10u,
        .dst_id = 0u,
        .session_id = 0x30u,
        .seq = 0x41u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };
    struct proto_packet decoded_packet = {0};
    uint8_t decoded_payload[sizeof(payload)] = {0};
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    uint64_t previous_hop_id = 0u;
    size_t payload_len = 0u;
    size_t written = 0u;

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0x20u,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_OK);
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_OK);
    assert(previous_hop_id == 0x10u);
    assert(decoded_packet.msg_type == packet.msg_type);
    assert(decoded_packet.src_id == packet.src_id);
    assert(payload_len == sizeof(payload));
    assert(memcmp(decoded_payload, payload, sizeof(payload)) == 0);

    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x21u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);

    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA56u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);
    frame[written - 1u] ^= 0x01u;
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_BAD_CRC);

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0x20u,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_OK);
    frame[UWB_MESH_FRAME_HEADER_LEN + 2u] = 0x01u;
    refresh_uwb_mesh_inner_and_outer_crc(frame, written);
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0x20u,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_OK);
    frame[UWB_MESH_FRAME_HEADER_LEN + 2u] = MSG_UWB_RANGE_SCHEDULE;
    refresh_uwb_mesh_inner_and_outer_crc(frame, written);
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0x20u,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_OK);
    frame[UWB_MESH_FRAME_HEADER_LEN + 2u] = 0x33u;
    refresh_uwb_mesh_inner_and_outer_crc(frame, written);
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x20u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_ERR_MALFORMED);

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0u,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_ERR_MALFORMED);
    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0x20u,
                                 &broadcast_packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_ERR_MALFORMED);

    assert(uwb_mesh_frame_encode(0xAA55u,
                                 0x10u,
                                 0u,
                                 &broadcast_packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &written) == PROTO_OK);
    assert(uwb_mesh_frame_decode(frame,
                                 written,
                                 0xAA55u,
                                 0x21u,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &payload_len) == PROTO_OK);
    assert(previous_hop_id == 0x10u);
    assert(decoded_packet.msg_type == broadcast_packet.msg_type);
    assert(decoded_packet.dst_id == 0u);
}

int main(void)
{
    test_poll_round_trip_diagnostic_not_click();
    test_response_final_and_report_round_trip();
    test_clicker_diag_round_trip();
    test_rejects_bad_clicker_diag();
    test_rejects_invalid_header_and_status();
    test_decode_rejects_wrong_type();
    test_decode_rejects_mismatched_full_id_short_address();
    test_decode_rejects_malformed_range_header_fields();
    test_wake_discovery_and_schedule_round_trip();
    test_control_frames_reject_bad_crc();
    test_wake_claim_decode_rejects_valid_crc_malformed_timing();
    test_discovery_decode_rejects_valid_crc_malformed_fields();
    test_range_release_decode_rejects_valid_crc_malformed_fields();
    test_range_schedule_decode_rejects_valid_crc_malformed_fields();
    test_schedule_rejects_unsafe_ranging_params();
    test_normal_click_schedule_requires_three_selected_anchors();
    test_schedule_samples_are_round_robin();
    test_anchor_epoch_arbitrates_and_locks();
    test_anchor_epoch_refreshes_repeated_same_attempt_claim();
    test_anchor_epoch_rejects_same_event_wrong_session_identity();
    test_claim_precedence_compare_uses_canonical_tuple();
    test_uwb_mesh_frame_round_trip_and_filters();
    return 0;
}
