#ifndef DWM3000_DRIVER_H
#define DWM3000_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

struct dwm3000_range_request {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t network_id;
    uint64_t session_nonce;
    uint16_t responder_short_addr;
    uint32_t session_id;
    uint8_t seq;
    uint8_t flags;
    uint32_t timeout_ms;
    bool capture_rsl;
};

struct dwm3000_range_result {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t session_id;
    uint8_t seq;
    uint8_t flags;
    int32_t distance_mm;
    uint8_t quality;
    int8_t rsl_dbm;
    uint8_t cir_sample[UWB_CIR_SAMPLE_LEN];
    bool rsl_sampled;
    bool cir_sampled;
    bool exchange_started;
    enum range_status status;
};

enum dwm3000_rx_failure {
    DWM3000_RX_FAILURE_NONE = 0,
    DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT = 1,
    DWM3000_RX_FAILURE_SFD_TIMEOUT = 2,
    DWM3000_RX_FAILURE_FRAME_TIMEOUT = 3,
    DWM3000_RX_FAILURE_CRC_OR_PHY = 4,
    DWM3000_RX_FAILURE_STS_QUALITY = 5,
    DWM3000_RX_FAILURE_BAD_FRAME = 6,
};

int dwm3000_driver_probe(uint32_t *dev_id);
int dwm3000_driver_initialise(bool idle_after_init);
int dwm3000_driver_configure_default(void);
int dwm3000_driver_configure_wake_mode(void);
int dwm3000_driver_standby(void);
int dwm3000_driver_send_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t timeout_ms);
int dwm3000_driver_receive_frame(uint32_t timeout_ms,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 size_t *frame_len,
                                 uint8_t *quality,
                                 int8_t *rsl_dbm);
int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure);
int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result);
int dwm3000_driver_responder_poll_expected(uint64_t local_anchor_id,
                                           const struct dwm3000_range_request *expected,
                                           uint32_t timeout_ms,
                                           struct dwm3000_range_result *result);
int dwm3000_driver_listen_activity(uint32_t timeout_ms, bool *activity_detected);

#endif
