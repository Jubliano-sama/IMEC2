#ifndef DWM3000_RUNTIME_H
#define DWM3000_RUNTIME_H

#include "dwm3000_timing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Devicetree values used by the production nRF52833 DWM3000 port. */
#define DWM3000_RUNTIME_SPI_SLOW_HZ 2000000u
#define DWM3000_RUNTIME_SPI_FAST_HZ 32000000u

/* Production driver/port waits, expressed in integer microseconds. */
#define DWM3000_RUNTIME_RESET_ASSERT_US 2000u
#define DWM3000_RUNTIME_RESET_RELEASE_US 5000u
#define DWM3000_RUNTIME_WAKE_ASSERT_US 500u
#define DWM3000_RUNTIME_WAKE_SETTLE_US 2000u
#define DWM3000_RUNTIME_SOFT_RESET_US 2000u
#define DWM3000_RUNTIME_PLL_LOCK_WORST_US 170u
#define DWM3000_RUNTIME_STATUS_POLL_PERIOD_US 50u

/*
 * Bench-derived conservative host-side costs. They include GPIO/controller
 * handoff and transaction setup, while the byte time is calculated from the
 * selected production SPI rate.
 */
#define DWM3000_RUNTIME_SPI_SWITCH_US 1u
#define DWM3000_RUNTIME_SPI_TRANSACTION_OVERHEAD_US 2u
#define DWM3000_RUNTIME_DEVICE_ID_TRANSFER_BYTES 8u
#define DWM3000_RUNTIME_RESTORE_TRANSFER_BYTES 64u
#define DWM3000_RUNTIME_CONFIG_TRANSFER_BYTES 96u
#define DWM3000_RUNTIME_RX_ARM_TRANSFER_BYTES 8u
#define DWM3000_RUNTIME_TX_START_TRANSFER_BYTES 6u
#define DWM3000_RUNTIME_STATUS_TRANSFER_BYTES 8u
#define DWM3000_RUNTIME_FRAME_IO_HEADER_BYTES 5u

/*
 * Conservative host-side bounds from the current RTT timing traces. The lean
 * range path includes response decoding, FINAL construction, and the control
 * register writes around it. Optional RX diagnostic/CIR collection has shown
 * an additional roughly 3 ms cost and must not run before delayed FINAL arm.
 */
#define DWM3000_RUNTIME_RANGE_PROCESS_WORST_US 2000u
#define DWM3000_RUNTIME_RX_DIAGNOSTICS_WORST_US 3000u

enum dwm3000_runtime_status {
    DWM3000_RUNTIME_OK = 0,
    DWM3000_RUNTIME_ERR_ARG = -2000,
    DWM3000_RUNTIME_ERR_BUSY = -2001,
    DWM3000_RUNTIME_ERR_SPI_ORDER = -2002,
    DWM3000_RUNTIME_ERR_RADIO_STATE = -2003,
    DWM3000_RUNTIME_ERR_NOT_READY = -2004,
    DWM3000_RUNTIME_ERR_OVERFLOW = -2005,
    DWM3000_RUNTIME_ERR_DEADLINE_MISSED = -2006,
};

enum dwm3000_runtime_spi_rate {
    DWM3000_RUNTIME_SPI_OFF = 0,
    DWM3000_RUNTIME_SPI_SLOW,
    DWM3000_RUNTIME_SPI_FAST,
};

enum dwm3000_runtime_radio_state {
    DWM3000_RUNTIME_RADIO_OFF = 0,
    DWM3000_RUNTIME_RADIO_IDLE,
    DWM3000_RUNTIME_RADIO_RX,
    DWM3000_RUNTIME_RADIO_TX,
    DWM3000_RUNTIME_RADIO_SLEEP,
};

enum dwm3000_runtime_operation {
    DWM3000_RUNTIME_OP_NONE = 0,
    DWM3000_RUNTIME_OP_SPI_SWITCH,
    DWM3000_RUNTIME_OP_RESET,
    DWM3000_RUNTIME_OP_WAKE,
    DWM3000_RUNTIME_OP_DEVICE_ID,
    DWM3000_RUNTIME_OP_RESTORE,
    DWM3000_RUNTIME_OP_CONFIGURE,
    DWM3000_RUNTIME_OP_PLL_LOCK,
    DWM3000_RUNTIME_OP_RX_ARM,
    DWM3000_RUNTIME_OP_TX_FRAME_WRITE,
    DWM3000_RUNTIME_OP_TX_START,
    DWM3000_RUNTIME_OP_STATUS_POLL,
    DWM3000_RUNTIME_OP_FRAME_READ,
    DWM3000_RUNTIME_OP_CIR_READ,
    DWM3000_RUNTIME_OP_RANGE_PROCESS,
    DWM3000_RUNTIME_OP_RX_DIAGNOSTICS,
    DWM3000_RUNTIME_OP_SLEEP,
};

struct dwm3000_runtime_interval {
    uint64_t start_us;
    uint64_t end_us;
    enum dwm3000_runtime_operation operation;
};

struct dwm3000_runtime {
    uint64_t cpu_busy_until_us;
    uint64_t spi_busy_until_us;
    uint64_t radio_busy_until_us;
    uint64_t rx_started_us;
    uint32_t operation_count;
    uint32_t illegal_operation_count;
    enum dwm3000_runtime_spi_rate spi_rate;
    enum dwm3000_runtime_radio_state radio_state;
    enum dwm3000_timing_phy configured_phy;
    enum dwm3000_runtime_operation last_operation;
    bool awake;
    bool configured;
    bool pll_locked;
    bool retained_common;
    bool retained_txrx;
    bool frame_written;
};

void dwm3000_runtime_init(struct dwm3000_runtime *runtime);
uint32_t dwm3000_runtime_spi_transfer_us(enum dwm3000_runtime_spi_rate rate,
                                        size_t transfer_bytes);
int dwm3000_runtime_set_spi_rate(struct dwm3000_runtime *runtime,
                                 enum dwm3000_runtime_spi_rate rate,
                                 uint64_t now_us,
                                 struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_reset(struct dwm3000_runtime *runtime,
                          uint64_t now_us,
                          struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_wake(struct dwm3000_runtime *runtime,
                         uint64_t now_us,
                         struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_read_device_id(struct dwm3000_runtime *runtime,
                                   uint64_t now_us,
                                   struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_restore_retained(struct dwm3000_runtime *runtime,
                                     uint64_t now_us,
                                     struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_configure(struct dwm3000_runtime *runtime,
                              enum dwm3000_timing_phy phy,
                              uint64_t now_us,
                              struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_lock_pll(struct dwm3000_runtime *runtime,
                             uint64_t now_us,
                             struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_prepare_phy(struct dwm3000_runtime *runtime,
                                enum dwm3000_timing_phy phy,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_arm_rx(struct dwm3000_runtime *runtime,
                           uint64_t now_us,
                           uint64_t rx_end_us,
                           struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_finish_rx(struct dwm3000_runtime *runtime,
                              uint64_t now_us);
int dwm3000_runtime_extend_rx(struct dwm3000_runtime *runtime,
                              uint64_t new_end_us);
int dwm3000_runtime_write_frame(struct dwm3000_runtime *runtime,
                                size_t frame_bytes_without_fcs,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_start_tx(struct dwm3000_runtime *runtime,
                             uint64_t now_us,
                             uint64_t air_end_us,
                             struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_start_delayed_tx(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    uint64_t scheduled_air_start_us,
    uint64_t air_end_us,
    struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_finish_tx(struct dwm3000_runtime *runtime,
                              uint64_t now_us);
int dwm3000_runtime_status_poll(struct dwm3000_runtime *runtime,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_read_frame(struct dwm3000_runtime *runtime,
                               size_t frame_bytes_without_fcs,
                               uint64_t now_us,
                               struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_read_cir(struct dwm3000_runtime *runtime,
                             size_t cir_bytes,
                             uint64_t now_us,
                             struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_process_range_frame(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_read_rx_diagnostics(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval);
int dwm3000_runtime_enter_retained_sleep(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval);

#ifdef __cplusplus
}
#endif

#endif
