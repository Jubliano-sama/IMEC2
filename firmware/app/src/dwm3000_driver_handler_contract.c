#include "dwm3000_driver.h"

#include "uwb_session.h"

#include <errno.h>
#include <stdint.h>

/* DW3000 timebase constants used by the production driver. */
#define DWM3000_DRIVER_UUS_TO_DWT_TIME UINT64_C(63898)
#define DWM3000_DRIVER_TIME_UNITS (1.0 / (499.2e6 * 128.0))
#define DWM3000_DRIVER_SPEED_OF_LIGHT_MPS 299702547.0

bool dwm3000_driver_header_matches_request(
    const struct uwb_range_header *header,
    const struct dwm3000_range_request *request,
    uint8_t expected_type)
{
    uint16_t responder_short_addr;

    if (header == NULL || request == NULL) {
        return false;
    }
    if (header->type != expected_type ||
        header->seq != request->seq ||
        header->round_index != request->round_index ||
        header->network_id != request->network_id ||
        header->session_id != request->session_id ||
        header->session_nonce != request->session_nonce ||
        header->initiator_short_addr !=
            uwb_session_short_addr_from_id(request->initiator_id) ||
        header->initiator_id != request->initiator_id ||
        header->flags != request->flags) {
        return false;
    }

    responder_short_addr = request->responder_short_addr != 0u ?
                           request->responder_short_addr :
                           uwb_session_short_addr_from_id(request->responder_id);
    return header->responder_short_addr == responder_short_addr &&
           header->responder_id == request->responder_id;
}

bool dwm3000_driver_final_matches_poll(
    const struct uwb_final_frame *final,
    const struct uwb_range_header *poll,
    uint64_t local_anchor_id)
{
    if (final == NULL || poll == NULL) {
        return false;
    }

    return final->header.seq == poll->seq &&
           final->header.round_index == poll->round_index &&
           final->header.network_id == poll->network_id &&
           final->header.session_id == poll->session_id &&
           final->header.session_nonce == poll->session_nonce &&
           final->header.initiator_short_addr == poll->initiator_short_addr &&
           final->header.responder_short_addr ==
               uwb_session_short_addr_from_id(local_anchor_id) &&
           final->header.flags == poll->flags &&
           final->header.initiator_id == poll->initiator_id &&
           final->header.responder_id == local_anchor_id;
}

uint16_t dwm3000_driver_dwt_delta_to_uus(uint32_t start_ts,
                                         uint32_t end_ts)
{
    uint32_t delta = end_ts - start_ts;
    uint32_t uus = (uint32_t)(((uint64_t)delta +
                               (DWM3000_DRIVER_UUS_TO_DWT_TIME / 2u)) /
                              DWM3000_DRIVER_UUS_TO_DWT_TIME);

    return uus > UINT16_MAX ? UINT16_MAX : (uint16_t)uus;
}

int dwm3000_driver_validate_reply_timing(uint16_t poll_to_resp_uus,
                                         uint16_t resp_to_final_uus,
                                         uint16_t expected_uus,
                                         uint16_t tolerance_uus)
{
    uint16_t poll_error_uus = poll_to_resp_uus > expected_uus ?
        poll_to_resp_uus - expected_uus : expected_uus - poll_to_resp_uus;
    uint16_t final_error_uus = resp_to_final_uus > expected_uus ?
        resp_to_final_uus - expected_uus : expected_uus - resp_to_final_uus;
    uint16_t reply_delta_uus = poll_to_resp_uus > resp_to_final_uus ?
        poll_to_resp_uus - resp_to_final_uus : resp_to_final_uus - poll_to_resp_uus;

    return (poll_error_uus > tolerance_uus ||
            final_error_uus > tolerance_uus ||
            reply_delta_uus > tolerance_uus) ?
           PROTO_ERR_MALFORMED : PROTO_OK;
}

int dwm3000_driver_compute_distance_mm(const struct uwb_final_frame *final,
                                       uint64_t poll_rx_ts,
                                       uint64_t resp_tx_ts,
                                       uint64_t final_rx_ts,
                                       int32_t *distance_mm)
{
    uint32_t poll_tx_ts_32;
    uint32_t resp_rx_ts_32;
    uint32_t final_tx_ts_32;
    uint32_t poll_rx_ts_32 = (uint32_t)poll_rx_ts;
    uint32_t resp_tx_ts_32 = (uint32_t)resp_tx_ts;
    uint32_t final_rx_ts_32 = (uint32_t)final_rx_ts;
    double ra;
    double rb;
    double da;
    double db;
    double denominator;
    double tof_dtu;
    double distance_m;
    double distance_mm_value;

    if (final == NULL || distance_mm == NULL) {
        return -EINVAL;
    }

    poll_tx_ts_32 = final->poll_tx_ts_32;
    resp_rx_ts_32 = final->resp_rx_ts_32;
    final_tx_ts_32 = final->final_tx_ts_32;
    ra = (double)(uint32_t)(resp_rx_ts_32 - poll_tx_ts_32);
    rb = (double)(uint32_t)(final_rx_ts_32 - resp_tx_ts_32);
    da = (double)(uint32_t)(final_tx_ts_32 - resp_rx_ts_32);
    db = (double)(uint32_t)(resp_tx_ts_32 - poll_rx_ts_32);
    denominator = ra + rb + da + db;

    if (denominator == 0.0) {
        return -EINVAL;
    }

    tof_dtu = ((ra * rb) - (da * db)) / denominator;
    if (tof_dtu < 0.0) {
        /*
         * A negative calibrated ToF is physically below the measurable
         * origin.  Preserve the otherwise-valid exchange as a zero-distance
         * sample instead of turning it into a ranging failure.
         */
        *distance_mm = 0;
        return 0;
    }
    distance_m = tof_dtu * DWM3000_DRIVER_TIME_UNITS *
                 DWM3000_DRIVER_SPEED_OF_LIGHT_MPS;
    distance_mm_value = distance_m * 1000.0;
    if (distance_mm_value > (double)INT32_MAX) {
        return -ERANGE;
    }
    *distance_mm = (int32_t)distance_mm_value;
    return 0;
}
