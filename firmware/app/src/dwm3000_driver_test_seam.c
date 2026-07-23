#include "dwm3000_driver_test_seam.h"

#include "uwb_session.h"

#include <errno.h>
#include <string.h>

static int decode_poll_with_optional_fcs(const uint8_t *frame,
                                         size_t frame_len,
                                         struct uwb_range_header *poll)
{
    int ret;

    ret = uwb_decode_poll(frame, frame_len, poll);
    if (ret == PROTO_OK) {
        return PROTO_OK;
    }
    if (frame_len <= UWB_PHY_FCS_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    ret = uwb_decode_poll(frame, frame_len - UWB_PHY_FCS_LEN, poll);
    return ret == PROTO_OK ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static int decode_final_with_optional_fcs(const uint8_t *frame,
                                          size_t frame_len,
                                          struct uwb_final_frame *final)
{
    int ret;

    ret = uwb_decode_final(frame, frame_len, final);
    if (ret == PROTO_OK) {
        return PROTO_OK;
    }
    if (frame_len <= UWB_PHY_FCS_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    ret = uwb_decode_final(frame, frame_len - UWB_PHY_FCS_LEN, final);
    return ret == PROTO_OK ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static void set_result_metadata(struct dwm3000_range_result *result,
                                const struct uwb_range_header *poll,
                                uint64_t local_anchor_id)
{
    if (result == NULL || poll == NULL) {
        return;
    }

    result->initiator_id = poll->initiator_id;
    result->responder_id = local_anchor_id;
    result->session_id = poll->session_id;
    result->seq = poll->seq;
    result->round_index = poll->round_index;
    result->flags = poll->flags;
}

int dwm3000_driver_test_evaluate_exchange(
    uint64_t local_anchor_id,
    const struct dwm3000_range_request *expected,
    const uint8_t *poll_frame,
    size_t poll_len,
    const uint8_t *final_frame,
    size_t final_len,
    uint32_t poll_rx_ts_32,
    uint32_t resp_tx_ts_32,
    uint32_t final_rx_ts_32,
    uint16_t reply_delay_uus,
    struct dwm3000_range_result *result,
    struct dwm3000_driver_exchange_evaluation *evaluation)
{
    struct uwb_range_header poll;
    struct uwb_final_frame final;
    int ret;

    if (local_anchor_id == 0u || expected == NULL || poll_frame == NULL ||
        poll_len == 0u || result == NULL || evaluation == NULL ||
        reply_delay_uus == 0u ||
        (expected->reply_delay_uus != 0u &&
         expected->reply_delay_uus != reply_delay_uus)) {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    result->status = RANGE_INTERNAL_ERROR;
    memset(evaluation, 0, sizeof(*evaluation));
    evaluation->status = RANGE_INTERNAL_ERROR;

    ret = decode_poll_with_optional_fcs(poll_frame, poll_len, &poll);
    if (ret != PROTO_OK) {
        result->status = RANGE_BAD_FRAME;
        evaluation->status = RANGE_BAD_FRAME;
        return -EBADMSG;
    }
    evaluation->poll_valid = true;
    set_result_metadata(result, &poll, local_anchor_id);
    if (poll.responder_short_addr !=
            uwb_session_short_addr_from_id(local_anchor_id) ||
        poll.responder_id != local_anchor_id ||
        !dwm3000_driver_header_matches_request(&poll,
                                               expected,
                                               MSG_UWB_POLL)) {
        result->status = RANGE_WRONG_TARGET;
        evaluation->status = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }
    result->exchange_started = true;

    if (final_frame == NULL || final_len == 0u) {
        result->status = RANGE_RX_TIMEOUT;
        evaluation->status = RANGE_RX_TIMEOUT;
        return -ETIMEDOUT;
    }

    ret = decode_final_with_optional_fcs(final_frame, final_len, &final);
    if (ret != PROTO_OK) {
        evaluation->final_received = true;
        result->status = RANGE_BAD_FRAME;
        evaluation->status = RANGE_BAD_FRAME;
        return -EBADMSG;
    }
    evaluation->final_received = true;
    if (!dwm3000_driver_final_matches_poll(&final,
                                           &poll,
                                           local_anchor_id)) {
        result->status = RANGE_WRONG_TARGET;
        evaluation->status = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }
    evaluation->final_valid = true;

    evaluation->poll_to_resp_uus = dwm3000_driver_dwt_delta_to_uus(
        poll_rx_ts_32,
        resp_tx_ts_32);
    evaluation->resp_to_final_uus = dwm3000_driver_dwt_delta_to_uus(
        final.resp_rx_ts_32,
        final.final_tx_ts_32);
    if (dwm3000_driver_validate_reply_timing(
            evaluation->poll_to_resp_uus,
            evaluation->resp_to_final_uus,
            reply_delay_uus,
            UWB_SESSION_REPLY_DELAY_TOLERANCE_US) != PROTO_OK) {
        result->status = RANGE_TIMING_INVALID;
        evaluation->status = RANGE_TIMING_INVALID;
        return -ETIME;
    }
    evaluation->timing_valid = true;

    ret = dwm3000_driver_compute_distance_mm(&final,
                                             poll_rx_ts_32,
                                             resp_tx_ts_32,
                                             final_rx_ts_32,
                                             &evaluation->distance_mm);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        evaluation->status = RANGE_INTERNAL_ERROR;
        return ret;
    }

    result->poll_rx_ts_32 = poll_rx_ts_32;
    result->resp_tx_ts_32 = resp_tx_ts_32;
    result->resp_rx_ts_32 = final.resp_rx_ts_32;
    result->final_tx_ts_32 = final.final_tx_ts_32;
    result->final_rx_ts_32 = final_rx_ts_32;
    result->distance_mm = evaluation->distance_mm;
    result->status = RANGE_OK;
    evaluation->status = RANGE_OK;
    return PROTO_OK;
}
