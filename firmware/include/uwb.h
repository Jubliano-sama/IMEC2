#ifndef UWB_H
#define UWB_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UWB_MARKER 0xCAu
#define UWB_VERSION 0x01u
#define UWB_HEADER_LEN 13u
#define UWB_POLL_LEN UWB_HEADER_LEN
#define UWB_RESP_LEN (UWB_HEADER_LEN + 8u)
#define UWB_FINAL_LEN (UWB_HEADER_LEN + 12u)
#define UWB_REPORT_LEN (UWB_HEADER_LEN + 8u)

struct uwb_range_header {
    uint8_t type;
    uint8_t seq;
    uint32_t session_id;
    uint16_t initiator_short_addr;
    uint16_t responder_short_addr;
    uint8_t flags;
};

struct uwb_response_frame {
    struct uwb_range_header header;
    uint32_t poll_rx_ts_32;
    uint32_t resp_tx_ts_32;
};

struct uwb_final_frame {
    struct uwb_range_header header;
    uint32_t poll_tx_ts_32;
    uint32_t resp_rx_ts_32;
    uint32_t final_tx_ts_32;
};

struct uwb_report_frame {
    struct uwb_range_header header;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status status;
    int8_t rsl_dbm;
};

bool uwb_frame_type_valid(uint8_t type);
int uwb_header_validate(const struct uwb_range_header *header, uint8_t expected_type);
int uwb_encode_poll(const struct uwb_range_header *header,
                         uint8_t *out,
                         size_t out_cap,
                         size_t *written);
int uwb_decode_poll(const uint8_t *data,
                         size_t len,
                         struct uwb_range_header *header);
int uwb_encode_response(const struct uwb_response_frame *frame,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written);
int uwb_decode_response(const uint8_t *data,
                             size_t len,
                             struct uwb_response_frame *frame);
int uwb_encode_final(const struct uwb_final_frame *frame,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written);
int uwb_decode_final(const uint8_t *data,
                          size_t len,
                          struct uwb_final_frame *frame);
int uwb_encode_report(const struct uwb_report_frame *frame,
                           uint8_t *out,
                           size_t out_cap,
                           size_t *written);
int uwb_decode_report(const uint8_t *data,
                           size_t len,
                           struct uwb_report_frame *frame);

#ifdef __cplusplus
}
#endif

#endif
