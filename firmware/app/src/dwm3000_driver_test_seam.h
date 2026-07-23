#ifndef DWM3000_DRIVER_TEST_SEAM_H
#define DWM3000_DRIVER_TEST_SEAM_H

#include "dwm3000_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This is deliberately a radio-independent evaluator, not a fake Zephyr
 * handler.  It covers the production decoder/identity/timing/distance
 * contract while leaving SPI, IRQ, delayed-TX admission, RX windows, and
 * report transmission to the exact-preset and hardware tests.
 */
struct dwm3000_driver_exchange_evaluation {
    bool poll_valid;
    bool final_received;
    bool final_valid;
    bool timing_valid;
    uint16_t poll_to_resp_uus;
    uint16_t resp_to_final_uus;
    int32_t distance_mm;
    enum range_status status;
};

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
    struct dwm3000_driver_exchange_evaluation *evaluation);

#endif
