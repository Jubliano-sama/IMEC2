#ifndef DWM3000_DRIVER_H
#define DWM3000_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uwb.h"

#define DWM3000_RX_DIAG_RAW_LEN 108u
#define DWM3000_CIR_SAMPLE_BYTES UWB_CIR_SAMPLE_LEN
#define DWM3000_CIR_ACCUM_SAMPLE_COUNT 2048u
#define DWM3000_CIR_WINDOW_PRE_SAMPLES 64u
#define DWM3000_CIR_WINDOW_SAMPLE_COUNT 192u
#define DWM3000_CIR_WINDOW_BYTES \
    (DWM3000_CIR_SAMPLE_BYTES * DWM3000_CIR_WINDOW_SAMPLE_COUNT)
#define DWM3000_FULL_CIR_SAMPLE_BYTES DWM3000_CIR_SAMPLE_BYTES
#define DWM3000_FULL_CIR_SAMPLE_COUNT DWM3000_CIR_WINDOW_SAMPLE_COUNT
#define DWM3000_FULL_CIR_BYTES \
    DWM3000_CIR_WINDOW_BYTES

struct dwm3000_range_request {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t network_id;
    uint64_t session_nonce;
    uint16_t responder_short_addr;
    uint32_t session_id;
    uint8_t seq;
    uint8_t round_index;
    uint8_t flags;
    uint32_t timeout_ms;
    uint16_t reply_delay_uus;
    bool capture_rsl;
    bool skip_responder_report;
    bool send_clicker_diag;
    bool expect_clicker_diag;
    bool expect_anchor_diag;
    bool send_anchor_diag;
    bool expect_anchor_diag_fragments;
    bool send_anchor_diag_fragments;
    uint8_t *anchor_full_cir;
    uint16_t anchor_full_cir_cap;
};

struct dwm3000_range_result {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t session_id;
    int64_t exchange_start_ms;
    uint8_t seq;
    uint8_t round_index;
    uint8_t flags;
    int32_t distance_mm;
    uint8_t quality;
    int8_t rsl_dbm;
    int16_t clock_offset_raw;
    int32_t carrier_integrator;
    uint32_t poll_tx_ts_32;
    uint32_t poll_rx_ts_32;
    uint32_t resp_tx_ts_32;
    uint32_t resp_rx_ts_32;
    uint32_t final_tx_ts_32;
    uint32_t final_rx_ts_32;
    uint8_t cir_sample[UWB_CIR_SAMPLE_LEN];
    uint8_t clicker_rx_diag_raw[DWM3000_RX_DIAG_RAW_LEN];
    uint8_t anchor_rx_diag_raw[DWM3000_RX_DIAG_RAW_LEN];
    uint8_t clicker_diag[UWB_CLICKER_DIAG_MAX_BYTES];
    uint16_t anchor_full_cir_len;
    uint16_t anchor_full_cir_total_len;
    uint16_t anchor_full_cir_first_path_index;
    uint16_t anchor_full_cir_start_index;
    uint8_t clicker_diag_len;
    uint8_t clicker_rx_diag_raw_len;
    uint8_t anchor_rx_diag_raw_len;
    uint32_t clicker_diag_status_flags;
    uint32_t anchor_diag_status_flags;
    uint32_t clicker_diag_status_detect_latency_us;
    bool rsl_sampled;
    bool cir_sampled;
    bool clicker_rx_diag_sampled;
    bool anchor_rx_diag_sampled;
    bool anchor_full_cir_sampled;
    bool anchor_full_cir_truncated;
    bool clock_offset_sampled;
    bool carrier_integrator_sampled;
    bool clicker_diag_received;
    bool clicker_diag_dropped;
    bool clicker_diag_truncated;
    bool exchange_started;
    enum range_status status;
};

enum dwm3000_rx_failure {
    DWM3000_RX_FAILURE_NONE = 0,
    DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT = 1,
    DWM3000_RX_FAILURE_SFD_TIMEOUT = 2,
    DWM3000_RX_FAILURE_FRAME_TIMEOUT = 3,
    DWM3000_RX_FAILURE_CRC_OR_PHY = 4,
    DWM3000_RX_FAILURE_BAD_FRAME = 5,
};

struct dwm3000_rx_frame_timing {
    uint64_t rx_timestamp;
    uint32_t rx_enable_time32;
    uint32_t rx_timestamp_time32;
    uint32_t rx_since_enable_uus;
    bool valid;
};

struct dwm3000_driver_stats {
    uint32_t sys_status_poll_loops;
    uint32_t sys_status_poll_timeouts;
    uint32_t sys_status_poll_max_duration_us;
    uint32_t sleep_wake_count;
    uint32_t sleep_wake_total_us;
    uint32_t sleep_wake_max_us;
    uint32_t sleep_wake_failures;
    uint32_t rx_starts;
    uint32_t rx_dones;
    uint32_t rx_timeouts;
    uint32_t rx_crc_failures;
    uint32_t rx_failures;
    uint32_t tx_starts;
    uint32_t tx_dones;
    uint32_t tx_failures;
};

int dwm3000_driver_probe(uint32_t *dev_id);
int dwm3000_driver_configure_default(void);
int dwm3000_driver_configure_range_mode(void);
int dwm3000_driver_configure_mesh_payload_mode(void);
int dwm3000_driver_configure_wake_mode(void);
int dwm3000_driver_idle(void);
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
int dwm3000_driver_receive_frame_continuous(uint32_t timeout_ms,
                                            uint8_t *frame,
                                            size_t frame_cap,
                                            size_t *frame_len,
                                            uint8_t *quality,
                                            int8_t *rsl_dbm,
                                            enum dwm3000_rx_failure *failure);
int dwm3000_driver_receive_frame_continuous_timed(uint32_t timeout_ms,
                                                  uint8_t *frame,
                                                  size_t frame_cap,
                                                  size_t *frame_len,
                                                  uint8_t *quality,
                                                  int8_t *rsl_dbm,
                                                  enum dwm3000_rx_failure *failure,
                                                  struct dwm3000_rx_frame_timing *timing);
int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result);
int dwm3000_driver_responder_poll_expected(uint64_t local_anchor_id,
                                           const struct dwm3000_range_request *expected,
                                           uint32_t timeout_ms,
                                           struct dwm3000_range_result *result);
void dwm3000_driver_stats_get(struct dwm3000_driver_stats *stats);

#endif
