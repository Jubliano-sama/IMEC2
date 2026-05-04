#include "uwb.h"

#include <assert.h>

static struct uwb_range_header header(uint8_t type, uint8_t flags)
{
    const struct uwb_range_header value = {
        .type = type,
        .seq = 17u,
        .session_id = 0xAABBCCDDu,
        .initiator_short_addr = 0x7788u,
        .responder_short_addr = 0x2211u,
        .flags = flags,
    };
    return value;
}

static void assert_same_header(const struct uwb_range_header *actual,
                               const struct uwb_range_header *expected)
{
    assert(actual->type == expected->type);
    assert(actual->seq == expected->seq);
    assert(actual->session_id == expected->session_id);
    assert(actual->initiator_short_addr == expected->initiator_short_addr);
    assert(actual->responder_short_addr == expected->responder_short_addr);
    assert(actual->flags == expected->flags);
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
    assert(UWB_POLL_LEN == 13u);
    assert(UWB_RESP_LEN == 21u);
    assert(UWB_FINAL_LEN == 25u);
    assert(UWB_REPORT_LEN == 21u);

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

static void test_rejects_invalid_header_and_status(void)
{
    struct uwb_range_header bad_header = header(MSG_UWB_POLL, FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK);
    struct uwb_report_frame bad_report = {
        .header = header(MSG_UWB_REPORT, FLAG_DIAGNOSTIC),
        .distance_mm = 0,
        .quality = 101u,
        .status = RANGE_OK,
    };
    uint8_t buf[UWB_REPORT_LEN];
    size_t written = 0u;

    assert(uwb_encode_poll(&bad_header, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
    assert(uwb_encode_report(&bad_report, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
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

int main(void)
{
    test_poll_round_trip_diagnostic_not_click();
    test_response_final_and_report_round_trip();
    test_rejects_invalid_header_and_status();
    test_decode_rejects_wrong_type();
    return 0;
}
