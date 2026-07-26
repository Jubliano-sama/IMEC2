#include "dwm3000_driver_test_seam.h"

#include "uwb_session.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_NETWORK UINT32_C(0x494d4543)
#define TEST_CLICKER UINT64_C(0xc100000000000001)
#define TEST_ANCHOR UINT64_C(0xc200000000000002)
#define TEST_EVENT UINT32_C(0x22070001)
#define TEST_NONCE UINT64_C(0x0102030405060708)
#define TEST_DWT_PER_UUS UINT32_C(63898)

static unsigned int failures;

#define CHECK(expr, ...) do {                                                \
    if (!(expr)) {                                                           \
        fprintf(stderr, "FAIL line=%d ", __LINE__);                         \
        fprintf(stderr, __VA_ARGS__);                                       \
        fputc('\n', stderr);                                                 \
        failures++;                                                          \
        return false;                                                        \
    }                                                                        \
} while (0)

struct handler_fixture {
    struct dwm3000_range_request request;
    struct uwb_range_header poll;
    struct uwb_final_frame final;
    uint32_t poll_rx_ts_32;
    uint32_t resp_tx_ts_32;
    uint32_t final_rx_ts_32;
    uint8_t poll_bytes[UWB_POLL_LEN];
    uint8_t final_bytes[UWB_FINAL_LEN];
    size_t poll_len;
    size_t final_len;
};

static bool build_fixture(struct handler_fixture *fixture)
{
    uint32_t round_trip_dwt = 213u;
    uint32_t reply_dwt = UWB_DS_TWR_REPLY_DELAY_US * TEST_DWT_PER_UUS;
    int ret;

    memset(fixture, 0, sizeof(*fixture));
    fixture->request = (struct dwm3000_range_request) {
        .initiator_id = TEST_CLICKER,
        .responder_id = TEST_ANCHOR,
        .network_id = TEST_NETWORK,
        .session_nonce = TEST_NONCE,
        .responder_short_addr = uwb_session_short_addr_from_id(TEST_ANCHOR),
        .session_id = TEST_EVENT,
        .seq = 1u,
        .round_index = 0u,
        .flags = FLAG_COUNT_AS_CLICK,
        .reply_delay_uus = UWB_DS_TWR_REPLY_DELAY_US,
        .capture_rsl = true,
    };
    fixture->poll = (struct uwb_range_header) {
        .type = MSG_UWB_POLL,
        .seq = fixture->request.seq,
        .round_index = fixture->request.round_index,
        .network_id = fixture->request.network_id,
        .session_id = fixture->request.session_id,
        .session_nonce = fixture->request.session_nonce,
        .initiator_short_addr = uwb_session_short_addr_from_id(TEST_CLICKER),
        .responder_short_addr = uwb_session_short_addr_from_id(TEST_ANCHOR),
        .flags = fixture->request.flags,
        .initiator_id = TEST_CLICKER,
        .responder_id = TEST_ANCHOR,
    };
    ret = uwb_encode_poll(&fixture->poll,
                          fixture->poll_bytes,
                          sizeof(fixture->poll_bytes),
                          &fixture->poll_len);
    CHECK(ret == PROTO_OK && fixture->poll_len == UWB_POLL_LEN,
          "POLL build failed ret=%d len=%zu", ret, fixture->poll_len);

    fixture->poll_rx_ts_32 = 200000u;
    fixture->resp_tx_ts_32 = fixture->poll_rx_ts_32 + reply_dwt;
    fixture->final.resp_rx_ts_32 = fixture->resp_tx_ts_32 + round_trip_dwt;
    fixture->final.final_tx_ts_32 = fixture->final.resp_rx_ts_32 + reply_dwt;
    fixture->final.header = fixture->poll;
    fixture->final.header.type = MSG_UWB_FINAL;
    fixture->final.poll_tx_ts_32 = fixture->poll_rx_ts_32 - round_trip_dwt;
    ret = uwb_encode_final(&fixture->final,
                           fixture->final_bytes,
                           sizeof(fixture->final_bytes),
                           &fixture->final_len);
    CHECK(ret == PROTO_OK && fixture->final_len == UWB_FINAL_LEN,
          "FINAL build failed ret=%d len=%zu", ret, fixture->final_len);
    fixture->final_rx_ts_32 = fixture->final.final_tx_ts_32 + round_trip_dwt;
    return true;
}

static bool evaluate_fixture(const struct handler_fixture *fixture,
                             const uint8_t *poll_bytes,
                             size_t poll_len,
                             const uint8_t *final_bytes,
                             size_t final_len,
                             struct dwm3000_range_result *result,
                             struct dwm3000_driver_exchange_evaluation *evaluation)
{
    int ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR,
        &fixture->request,
        poll_bytes,
        poll_len,
        final_bytes,
        final_len,
        fixture->poll_rx_ts_32,
        fixture->resp_tx_ts_32,
        fixture->final_rx_ts_32,
        fixture->request.reply_delay_uus,
        result,
        evaluation);

    CHECK(ret == PROTO_OK && result->status == RANGE_OK &&
              evaluation->status == RANGE_OK && evaluation->poll_valid &&
              result->exchange_started && evaluation->final_received &&
              evaluation->final_valid &&
              evaluation->timing_valid &&
              evaluation->poll_to_resp_uus == fixture->request.reply_delay_uus &&
              evaluation->resp_to_final_uus == fixture->request.reply_delay_uus,
          "valid exchange rejected ret=%d status=%u poll=%u final=%u timing=%u/%u",
          ret,
          result->status,
          evaluation->poll_valid ? 1u : 0u,
          evaluation->final_received ? 1u : 0u,
          evaluation->poll_to_resp_uus,
          evaluation->resp_to_final_uus);
    CHECK(result->distance_mm >= 900 && result->distance_mm <= 1100,
          "valid exchange produced an unexpected shared distance: %d",
          result->distance_mm);
    return true;
}

static bool test_valid_exchange_and_header_contract(void)
{
    struct handler_fixture fixture;
    struct dwm3000_range_result result;
    struct dwm3000_driver_exchange_evaluation evaluation;
    struct uwb_response_frame response;
    uint32_t wrap_start = UINT32_MAX - 100u;
    uint32_t wrap_end = wrap_start + 2u * TEST_DWT_PER_UUS;

    CHECK(build_fixture(&fixture), "fixture construction failed");
    CHECK(evaluate_fixture(&fixture,
                           fixture.poll_bytes,
                           fixture.poll_len,
                           fixture.final_bytes,
                           fixture.final_len,
                           &result,
                           &evaluation),
          "valid exchange evaluation failed");

    response.header = fixture.poll;
    response.header.type = MSG_UWB_RESP;
    response.header.responder_short_addr =
        uwb_session_short_addr_from_id(TEST_ANCHOR);
    response.header.responder_id = TEST_ANCHOR;
    response.poll_rx_ts_32 = fixture.poll_rx_ts_32;
    response.resp_tx_ts_32 = fixture.resp_tx_ts_32;
    CHECK(dwm3000_driver_header_matches_request(&response.header,
                                                &fixture.request,
                                                MSG_UWB_RESP),
          "RESP header was rejected by the shared production predicate");
    CHECK(dwm3000_driver_dwt_delta_to_uus(
              fixture.poll_rx_ts_32,
              fixture.resp_tx_ts_32) == fixture.request.reply_delay_uus,
          "DWT-to-UUS conversion did not preserve the configured reply delay");
    CHECK(dwm3000_driver_dwt_delta_to_uus(wrap_start, wrap_end) == 2u,
          "timestamp wrap arithmetic was not exercised as a modular delta");
    return true;
}

static bool test_timeout_and_malformed_frames(void)
{
    struct handler_fixture fixture;
    struct dwm3000_range_result result;
    struct dwm3000_driver_exchange_evaluation evaluation;
    uint8_t malformed_poll[UWB_POLL_LEN];
    uint8_t malformed_final[UWB_FINAL_LEN];
    int ret;

    CHECK(build_fixture(&fixture), "fixture construction failed");
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, fixture.poll_bytes, fixture.poll_len,
        NULL, 0u, fixture.poll_rx_ts_32, fixture.resp_tx_ts_32,
        fixture.final_rx_ts_32,
        fixture.request.reply_delay_uus, &result, &evaluation);
    CHECK(ret == -ETIMEDOUT && result.status == RANGE_RX_TIMEOUT &&
              result.exchange_started && evaluation.poll_valid &&
              !evaluation.final_received,
          "missing FINAL was not classified as timeout ret=%d status=%u",
          ret, result.status);

    memcpy(malformed_poll, fixture.poll_bytes, sizeof(malformed_poll));
    malformed_poll[0] ^= 0x01u;
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, malformed_poll, sizeof(malformed_poll),
        fixture.final_bytes, fixture.final_len, fixture.poll_rx_ts_32,
        fixture.resp_tx_ts_32, fixture.final_rx_ts_32,
        fixture.request.reply_delay_uus, &result,
        &evaluation);
    CHECK(ret == -EBADMSG && result.status == RANGE_BAD_FRAME &&
              !evaluation.poll_valid && !evaluation.final_received,
          "malformed POLL was not rejected before exchange evaluation ret=%d status=%u",
          ret, result.status);

    memcpy(malformed_final, fixture.final_bytes, sizeof(malformed_final));
    malformed_final[0] ^= 0x01u;
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, fixture.poll_bytes, fixture.poll_len,
        malformed_final, sizeof(malformed_final), fixture.poll_rx_ts_32,
        fixture.resp_tx_ts_32, fixture.final_rx_ts_32,
        fixture.request.reply_delay_uus, &result,
        &evaluation);
    CHECK(ret == -EBADMSG && result.status == RANGE_BAD_FRAME &&
              evaluation.poll_valid && evaluation.final_received &&
              !evaluation.final_valid,
          "malformed FINAL was not rejected after a valid POLL ret=%d status=%u",
          ret, result.status);
    return true;
}

static bool test_wrong_target_timing_and_fcs(void)
{
    struct handler_fixture fixture;
    struct dwm3000_range_result result;
    struct dwm3000_driver_exchange_evaluation evaluation;
    uint8_t wrong_target[UWB_POLL_LEN];
    uint8_t bad_timing[UWB_FINAL_LEN];
    uint8_t negative_distance_final[UWB_FINAL_LEN];
    uint8_t poll_with_fcs[UWB_POLL_LEN + UWB_PHY_FCS_LEN];
    uint8_t final_with_fcs[UWB_FINAL_LEN + UWB_PHY_FCS_LEN];
    struct uwb_range_header wrong_header;
    struct dwm3000_range_request wrong_local_request;
    struct uwb_final_frame bad_final;
    struct uwb_final_frame negative_final;
    size_t length;
    int ret;

    CHECK(build_fixture(&fixture), "fixture construction failed");
    wrong_header = fixture.poll;
    wrong_header.responder_id = UINT64_C(0xc400000000000004);
    wrong_header.responder_short_addr =
        uwb_session_short_addr_from_id(wrong_header.responder_id);
    CHECK(uwb_encode_poll(&wrong_header,
                          wrong_target,
                          sizeof(wrong_target),
                          &length) == PROTO_OK,
          "wrong-target POLL build failed");
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, wrong_target, length,
        fixture.final_bytes, fixture.final_len, fixture.poll_rx_ts_32,
        fixture.resp_tx_ts_32, fixture.final_rx_ts_32,
        fixture.request.reply_delay_uus, &result,
        &evaluation);
    CHECK(ret == -EADDRNOTAVAIL && result.status == RANGE_WRONG_TARGET &&
              evaluation.poll_valid && !evaluation.final_received,
          "wrong-target POLL was accepted ret=%d status=%u",
          ret, result.status);

    wrong_local_request = fixture.request;
    wrong_local_request.responder_id = UINT64_C(0xc400000000000004);
    wrong_local_request.responder_short_addr =
        uwb_session_short_addr_from_id(wrong_local_request.responder_id);
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &wrong_local_request, wrong_target, length,
        fixture.final_bytes, fixture.final_len, fixture.poll_rx_ts_32,
        fixture.resp_tx_ts_32, fixture.final_rx_ts_32,
        wrong_local_request.reply_delay_uus, &result, &evaluation);
    CHECK(ret == -EADDRNOTAVAIL && result.status == RANGE_WRONG_TARGET &&
              evaluation.poll_valid && !evaluation.final_received,
          "POLL for a non-local anchor was admitted by the evaluator ret=%d status=%u",
          ret, result.status);

    bad_final = fixture.final;
    bad_final.final_tx_ts_32 += 100u * TEST_DWT_PER_UUS;
    CHECK(uwb_encode_final(&bad_final,
                           bad_timing,
                           sizeof(bad_timing),
                           &length) == PROTO_OK,
          "bad-timing FINAL build failed");
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, fixture.poll_bytes, fixture.poll_len,
        bad_timing, length, fixture.poll_rx_ts_32, fixture.resp_tx_ts_32,
        fixture.final_rx_ts_32, fixture.request.reply_delay_uus,
        &result, &evaluation);
    CHECK(ret == -ETIME && result.status == RANGE_TIMING_INVALID &&
              evaluation.final_valid && !evaluation.timing_valid,
          "bad DS-TWR timing was not rejected ret=%d status=%u",
          ret, result.status);

    negative_final = fixture.final;
    negative_final.poll_tx_ts_32 = 1000000u;
    negative_final.resp_rx_ts_32 = negative_final.poll_tx_ts_32 + 100u;
    negative_final.final_tx_ts_32 =
        negative_final.resp_rx_ts_32 +
        fixture.request.reply_delay_uus * TEST_DWT_PER_UUS;
    CHECK(uwb_encode_final(&negative_final,
                           negative_distance_final,
                           sizeof(negative_distance_final),
                           &length) == PROTO_OK,
          "negative-distance FINAL build failed");
    ret = dwm3000_driver_test_evaluate_exchange(
        TEST_ANCHOR, &fixture.request, fixture.poll_bytes, fixture.poll_len,
        negative_distance_final, length, 1000u,
        1000u + fixture.request.reply_delay_uus * TEST_DWT_PER_UUS,
        1001u + fixture.request.reply_delay_uus * TEST_DWT_PER_UUS,
        fixture.request.reply_delay_uus, &result, &evaluation);
    CHECK(ret == -ERANGE && result.status == RANGE_INTERNAL_ERROR &&
              evaluation.final_valid && evaluation.timing_valid &&
              evaluation.status == RANGE_INTERNAL_ERROR,
          "negative ToF was reported as a valid distance ret=%d status=%u",
          ret, result.status);

    memcpy(poll_with_fcs, fixture.poll_bytes, fixture.poll_len);
    memcpy(&poll_with_fcs[fixture.poll_len], "\x00\x00", UWB_PHY_FCS_LEN);
    memcpy(final_with_fcs, fixture.final_bytes, fixture.final_len);
    memcpy(&final_with_fcs[fixture.final_len], "\x00\x00", UWB_PHY_FCS_LEN);
    CHECK(evaluate_fixture(&fixture,
                           poll_with_fcs,
                           fixture.poll_len + UWB_PHY_FCS_LEN,
                           final_with_fcs,
                           fixture.final_len + UWB_PHY_FCS_LEN,
                           &result,
                           &evaluation),
          "optional-FCS exchange was not accepted");
    return true;
}

static bool test_hardware_negative_tof_trace(void)
{
    /*
     * Raw timestamps from a real two-anchor survey where the antennas were
     * close enough for the calibrated exchange to produce a negative ToF.
     * Keep this exact trace at the production driver boundary: it must never
     * be rounded or clamped into RANGE_OK with a zero distance.
     */
    struct uwb_final_frame final = {
        .poll_tx_ts_32 = UINT32_C(0x1e9af801),
        .resp_rx_ts_32 = UINT32_C(0x3d13459b),
        .final_tx_ts_32 = UINT32_C(0x5b8b9201),
    };
    int32_t distance_mm = INT32_C(12345);
    int ret;

    ret = dwm3000_driver_compute_distance_mm(
        &final,
        UINT32_C(0x7888c019),
        UINT32_C(0x97010c01),
        UINT32_C(0xb5795695),
        &distance_mm);
    CHECK(ret == -ERANGE && distance_mm == INT32_C(12345),
          "negative-ToF hardware trace became a usable distance ret=%d distance=%d",
          ret,
          distance_mm);
    return true;
}

int main(void)
{
    bool ok = true;

    ok = test_valid_exchange_and_header_contract() && ok;
    ok = test_timeout_and_malformed_frames() && ok;
    ok = test_wrong_target_timing_and_fcs() && ok;
    ok = test_hardware_negative_tof_trace() && ok;
    if (!ok || failures != 0u) {
        fprintf(stderr, "DS-TWR production semantic contract: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("PASS DS-TWR production semantic contract (decode/identity/timing/distance)");
    return 0;
}
