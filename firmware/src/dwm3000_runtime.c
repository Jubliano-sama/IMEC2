#include "dwm3000_runtime.h"

#include "uwb_rf_scope.h"

#include <limits.h>
#include <string.h>

static int runtime_fail(struct dwm3000_runtime *runtime, int error)
{
    if (runtime != NULL && runtime->illegal_operation_count < UINT32_MAX) {
        runtime->illegal_operation_count++;
    }
    return error;
}

static void note_interval(struct dwm3000_runtime *runtime,
                          enum dwm3000_runtime_operation operation,
                          uint64_t start_us,
                          uint64_t end_us,
                          struct dwm3000_runtime_interval *interval)
{
    runtime->last_operation = operation;
    runtime->operation_count++;
    runtime->cpu_busy_until_us = end_us;
    if (interval != NULL) {
        interval->start_us = start_us;
        interval->end_us = end_us;
        interval->operation = operation;
    }
}

static int begin_idle_operation(struct dwm3000_runtime *runtime,
                                uint64_t now_us)
{
    if (runtime == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (now_us < runtime->cpu_busy_until_us ||
        now_us < runtime->spi_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_BUSY);
    }
    if (runtime->radio_state == DWM3000_RUNTIME_RADIO_RX ||
        runtime->radio_state == DWM3000_RUNTIME_RADIO_TX ||
        now_us < runtime->radio_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_RADIO_STATE);
    }
    return DWM3000_RUNTIME_OK;
}

static int run_spi_operation(struct dwm3000_runtime *runtime,
                             enum dwm3000_runtime_spi_rate required_rate,
                             enum dwm3000_runtime_operation operation,
                             size_t transfer_bytes,
                             uint64_t now_us,
                             bool require_idle_radio,
                             struct dwm3000_runtime_interval *interval)
{
    uint32_t duration_us;
    int ret;

    if (runtime == NULL || transfer_bytes == 0u) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (require_idle_radio) {
        ret = begin_idle_operation(runtime, now_us);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
    } else if (now_us < runtime->cpu_busy_until_us ||
               now_us < runtime->spi_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_BUSY);
    }
    if (runtime->spi_rate != required_rate) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_SPI_ORDER);
    }
    duration_us = dwm3000_runtime_spi_transfer_us(required_rate,
                                                  transfer_bytes);
    if (duration_us == 0u || now_us > UINT64_MAX - duration_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_OVERFLOW);
    }
    runtime->spi_busy_until_us = now_us + duration_us;
    note_interval(runtime, operation, now_us, now_us + duration_us, interval);
    return DWM3000_RUNTIME_OK;
}

static int run_scoped_frame_spi_operation(
    struct dwm3000_runtime *runtime,
    enum dwm3000_runtime_operation operation,
    size_t frame_bytes_without_fcs,
    uint64_t now_us,
    bool require_idle_radio,
    struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval local_interval;
    struct dwm3000_runtime_interval *result_interval =
        interval == NULL ? &local_interval : interval;
    uint64_t extended_end_us;
    int ret;

    /* The production driver reads/writes the scope and protocol bytes as two
     * SPI transactions. Model both register headers and the second handoff. */
    ret = run_spi_operation(
        runtime,
        DWM3000_RUNTIME_SPI_FAST,
        operation,
        frame_bytes_without_fcs + UWB_RF_SCOPE_WIRE_LEN +
            (2u * DWM3000_RUNTIME_FRAME_IO_HEADER_BYTES),
        now_us,
        require_idle_radio,
        result_interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    extended_end_us = result_interval->end_us +
                      DWM3000_RUNTIME_SPI_TRANSACTION_OVERHEAD_US + 1u;
    if (extended_end_us < result_interval->end_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_OVERFLOW);
    }
    runtime->spi_busy_until_us = extended_end_us;
    runtime->cpu_busy_until_us = extended_end_us;
    result_interval->end_us = extended_end_us;
    return DWM3000_RUNTIME_OK;
}

void dwm3000_runtime_init(struct dwm3000_runtime *runtime)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->radio_state = DWM3000_RUNTIME_RADIO_OFF;
    runtime->configured_phy = DWM3000_TIMING_PHY_CH5_WAKE;
}

uint32_t dwm3000_runtime_spi_transfer_us(enum dwm3000_runtime_spi_rate rate,
                                        size_t transfer_bytes)
{
    uint32_t hz;
    uint64_t bit_time_us;
    uint64_t total_us;

    if (transfer_bytes == 0u) {
        return 0u;
    }
    hz = rate == DWM3000_RUNTIME_SPI_SLOW ? DWM3000_RUNTIME_SPI_SLOW_HZ :
         rate == DWM3000_RUNTIME_SPI_FAST ? DWM3000_RUNTIME_SPI_FAST_HZ : 0u;
    if (hz == 0u || transfer_bytes > UINT64_MAX / 8u) {
        return 0u;
    }
    bit_time_us = ((uint64_t)transfer_bytes * 8u * 1000000u + hz - 1u) / hz;
    total_us = bit_time_us + DWM3000_RUNTIME_SPI_TRANSACTION_OVERHEAD_US;
    return total_us > UINT32_MAX ? UINT32_MAX : (uint32_t)total_us;
}

int dwm3000_runtime_set_spi_rate(struct dwm3000_runtime *runtime,
                                 enum dwm3000_runtime_spi_rate rate,
                                 uint64_t now_us,
                                 struct dwm3000_runtime_interval *interval)
{
    int ret;

    if (rate != DWM3000_RUNTIME_SPI_SLOW &&
        rate != DWM3000_RUNTIME_SPI_FAST) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    ret = begin_idle_operation(runtime, now_us);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (runtime->spi_rate == rate) {
        note_interval(runtime,
                      DWM3000_RUNTIME_OP_SPI_SWITCH,
                      now_us,
                      now_us,
                      interval);
        return DWM3000_RUNTIME_OK;
    }
    if (now_us > UINT64_MAX - DWM3000_RUNTIME_SPI_SWITCH_US) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_OVERFLOW);
    }
    runtime->spi_rate = rate;
    runtime->spi_busy_until_us = now_us + DWM3000_RUNTIME_SPI_SWITCH_US;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_SPI_SWITCH,
                  now_us,
                  now_us + DWM3000_RUNTIME_SPI_SWITCH_US,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_reset(struct dwm3000_runtime *runtime,
                          uint64_t now_us,
                          struct dwm3000_runtime_interval *interval)
{
    const uint32_t duration_us = DWM3000_RUNTIME_RESET_ASSERT_US +
                                 DWM3000_RUNTIME_RESET_RELEASE_US;
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (runtime->spi_rate != DWM3000_RUNTIME_SPI_SLOW) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_SPI_ORDER);
    }
    runtime->awake = true;
    runtime->configured = false;
    runtime->pll_locked = false;
    runtime->retained_common = false;
    runtime->retained_txrx = false;
    runtime->frame_written = false;
    runtime->radio_state = DWM3000_RUNTIME_RADIO_IDLE;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_RESET,
                  now_us,
                  now_us + duration_us,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_wake(struct dwm3000_runtime *runtime,
                         uint64_t now_us,
                         struct dwm3000_runtime_interval *interval)
{
    const uint32_t duration_us = DWM3000_RUNTIME_WAKE_ASSERT_US +
                                 DWM3000_RUNTIME_WAKE_SETTLE_US;
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (runtime->spi_rate != DWM3000_RUNTIME_SPI_SLOW) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_SPI_ORDER);
    }
    if (runtime->radio_state != DWM3000_RUNTIME_RADIO_SLEEP ||
        !runtime->retained_common || !runtime->retained_txrx) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_RADIO_STATE);
    }
    runtime->awake = true;
    runtime->pll_locked = false;
    runtime->radio_state = DWM3000_RUNTIME_RADIO_IDLE;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_WAKE,
                  now_us,
                  now_us + duration_us,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_read_device_id(struct dwm3000_runtime *runtime,
                                   uint64_t now_us,
                                   struct dwm3000_runtime_interval *interval)
{
    if (runtime == NULL || !runtime->awake) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    return run_spi_operation(runtime,
                             runtime->configured ? DWM3000_RUNTIME_SPI_FAST :
                                                   DWM3000_RUNTIME_SPI_SLOW,
                             DWM3000_RUNTIME_OP_DEVICE_ID,
                             DWM3000_RUNTIME_DEVICE_ID_TRANSFER_BYTES,
                             now_us,
                             true,
                             interval);
}

int dwm3000_runtime_restore_retained(struct dwm3000_runtime *runtime,
                                     uint64_t now_us,
                                     struct dwm3000_runtime_interval *interval)
{
    int ret;

    if (runtime == NULL || !runtime->awake || !runtime->retained_common ||
        !runtime->retained_txrx) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_SLOW,
                            DWM3000_RUNTIME_OP_RESTORE,
                            DWM3000_RUNTIME_RESTORE_TRANSFER_BYTES,
                            now_us,
                            true,
                            interval);
    if (ret == DWM3000_RUNTIME_OK) {
        runtime->configured = true;
    }
    return ret;
}

int dwm3000_runtime_configure(struct dwm3000_runtime *runtime,
                              enum dwm3000_timing_phy phy,
                              uint64_t now_us,
                              struct dwm3000_runtime_interval *interval)
{
    int ret;

    if (runtime == NULL || !runtime->awake ||
        dwm3000_timing_phy_profile(phy) == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_FAST,
                            DWM3000_RUNTIME_OP_CONFIGURE,
                            DWM3000_RUNTIME_CONFIG_TRANSFER_BYTES,
                            now_us,
                            true,
                            interval);
    if (ret == DWM3000_RUNTIME_OK) {
        runtime->configured = true;
        runtime->configured_phy = phy;
        runtime->pll_locked = false;
    }
    return ret;
}

int dwm3000_runtime_lock_pll(struct dwm3000_runtime *runtime,
                             uint64_t now_us,
                             struct dwm3000_runtime_interval *interval)
{
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (!runtime->awake || !runtime->configured ||
        runtime->spi_rate != DWM3000_RUNTIME_SPI_FAST) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    runtime->pll_locked = true;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_PLL_LOCK,
                  now_us,
                  now_us + DWM3000_RUNTIME_PLL_LOCK_WORST_US,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_prepare_phy(struct dwm3000_runtime *runtime,
                                enum dwm3000_timing_phy phy,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval step;
    uint64_t cursor = now_us;
    int ret;

    if (runtime == NULL || dwm3000_timing_phy_profile(phy) == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (runtime->awake && runtime->configured && runtime->pll_locked &&
        runtime->configured_phy == phy &&
        runtime->radio_state == DWM3000_RUNTIME_RADIO_IDLE) {
        note_interval(runtime, DWM3000_RUNTIME_OP_CONFIGURE, now_us, now_us,
                      interval);
        return DWM3000_RUNTIME_OK;
    }

    if (runtime->spi_rate != DWM3000_RUNTIME_SPI_SLOW) {
        ret = dwm3000_runtime_set_spi_rate(runtime,
                                           DWM3000_RUNTIME_SPI_SLOW,
                                           cursor,
                                           &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
    }

    if (runtime->radio_state == DWM3000_RUNTIME_RADIO_SLEEP &&
        runtime->retained_common && runtime->retained_txrx &&
        runtime->configured_phy == phy) {
        ret = dwm3000_runtime_wake(runtime, cursor, &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
        ret = dwm3000_runtime_restore_retained(runtime, cursor, &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
    } else {
        ret = dwm3000_runtime_reset(runtime, cursor, &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
        ret = dwm3000_runtime_read_device_id(runtime, cursor, &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
    }

    ret = dwm3000_runtime_set_spi_rate(runtime,
                                       DWM3000_RUNTIME_SPI_FAST,
                                       cursor,
                                       &step);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = step.end_us;
    if (runtime->configured_phy != phy || !runtime->configured) {
        ret = dwm3000_runtime_configure(runtime, phy, cursor, &step);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = step.end_us;
    }
    ret = dwm3000_runtime_read_device_id(runtime, cursor, &step);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = step.end_us;
    ret = dwm3000_runtime_lock_pll(runtime, cursor, &step);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = step.end_us;
    if (interval != NULL) {
        interval->start_us = now_us;
        interval->end_us = cursor;
        interval->operation = DWM3000_RUNTIME_OP_CONFIGURE;
    }
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_arm_rx(struct dwm3000_runtime *runtime,
                           uint64_t now_us,
                           uint64_t rx_end_us,
                           struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval spi_interval;
    int ret;

    if (runtime == NULL || rx_end_us <= now_us) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (!runtime->awake || !runtime->configured || !runtime->pll_locked) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_FAST,
                            DWM3000_RUNTIME_OP_RX_ARM,
                            DWM3000_RUNTIME_RX_ARM_TRANSFER_BYTES,
                            now_us,
                            true,
                            &spi_interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (rx_end_us <= spi_interval.end_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_ARG);
    }
    runtime->radio_state = DWM3000_RUNTIME_RADIO_RX;
    runtime->rx_started_us = spi_interval.end_us;
    runtime->radio_busy_until_us = rx_end_us;
    if (interval != NULL) {
        interval->start_us = spi_interval.end_us;
        interval->end_us = rx_end_us;
        interval->operation = DWM3000_RUNTIME_OP_RX_ARM;
    }
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_finish_rx(struct dwm3000_runtime *runtime,
                              uint64_t now_us)
{
    if (runtime == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (runtime->radio_state != DWM3000_RUNTIME_RADIO_RX ||
        now_us < runtime->rx_started_us || now_us > runtime->radio_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_RADIO_STATE);
    }
    runtime->radio_state = DWM3000_RUNTIME_RADIO_IDLE;
    runtime->radio_busy_until_us = now_us;
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_extend_rx(struct dwm3000_runtime *runtime,
                              uint64_t new_end_us)
{
    if (runtime == NULL || runtime->radio_state != DWM3000_RUNTIME_RADIO_RX ||
        new_end_us <= runtime->radio_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_RADIO_STATE);
    }
    runtime->radio_busy_until_us = new_end_us;
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_write_frame(struct dwm3000_runtime *runtime,
                                size_t frame_bytes_without_fcs,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval)
{
    const struct dwm3000_phy_timing *profile;
    int ret;

    if (runtime == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    profile = dwm3000_timing_phy_profile(runtime->configured_phy);
    if (!runtime->awake || !runtime->configured || !runtime->pll_locked ||
        profile == NULL || frame_bytes_without_fcs == 0u ||
        frame_bytes_without_fcs >
            profile->max_frame_bytes_without_fcs - UWB_RF_SCOPE_WIRE_LEN) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    ret = run_scoped_frame_spi_operation(
        runtime,
        DWM3000_RUNTIME_OP_TX_FRAME_WRITE,
        frame_bytes_without_fcs,
        now_us,
        true,
        interval);
    if (ret == DWM3000_RUNTIME_OK) {
        runtime->frame_written = true;
    }
    return ret;
}

int dwm3000_runtime_start_tx(struct dwm3000_runtime *runtime,
                             uint64_t now_us,
                             uint64_t air_end_us,
                             struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval spi_interval;
    int ret;

    if (runtime == NULL || !runtime->frame_written) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_FAST,
                            DWM3000_RUNTIME_OP_TX_START,
                            DWM3000_RUNTIME_TX_START_TRANSFER_BYTES,
                            now_us,
                            true,
                            &spi_interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (air_end_us <= spi_interval.end_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_ARG);
    }
    runtime->radio_state = DWM3000_RUNTIME_RADIO_TX;
    runtime->radio_busy_until_us = air_end_us;
    runtime->frame_written = false;
    if (interval != NULL) {
        interval->start_us = spi_interval.end_us;
        interval->end_us = air_end_us;
        interval->operation = DWM3000_RUNTIME_OP_TX_START;
    }
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_start_delayed_tx(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    uint64_t scheduled_air_start_us,
    uint64_t air_end_us,
    struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval spi_interval;
    int ret;

    if (runtime == NULL || !runtime->frame_written) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    if (scheduled_air_start_us >= air_end_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_ARG);
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_FAST,
                            DWM3000_RUNTIME_OP_TX_START,
                            DWM3000_RUNTIME_TX_START_TRANSFER_BYTES,
                            now_us,
                            true,
                            &spi_interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (spi_interval.end_us >= scheduled_air_start_us) {
        if (interval != NULL) {
            *interval = spi_interval;
        }
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_DEADLINE_MISSED);
    }

    runtime->radio_state = DWM3000_RUNTIME_RADIO_TX;
    runtime->radio_busy_until_us = air_end_us;
    runtime->frame_written = false;
    if (interval != NULL) {
        interval->start_us = scheduled_air_start_us;
        interval->end_us = air_end_us;
        interval->operation = DWM3000_RUNTIME_OP_TX_START;
    }
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_finish_tx(struct dwm3000_runtime *runtime,
                              uint64_t now_us)
{
    if (runtime == NULL) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    if (runtime->radio_state != DWM3000_RUNTIME_RADIO_TX ||
        now_us < runtime->radio_busy_until_us) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_RADIO_STATE);
    }
    runtime->radio_state = DWM3000_RUNTIME_RADIO_IDLE;
    runtime->radio_busy_until_us = now_us;
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_status_poll(struct dwm3000_runtime *runtime,
                                uint64_t now_us,
                                struct dwm3000_runtime_interval *interval)
{
    struct dwm3000_runtime_interval spi_interval;
    uint64_t end_us;
    int ret;

    if (runtime == NULL || !runtime->awake) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    ret = run_spi_operation(runtime,
                            DWM3000_RUNTIME_SPI_FAST,
                            DWM3000_RUNTIME_OP_STATUS_POLL,
                            DWM3000_RUNTIME_STATUS_TRANSFER_BYTES,
                            now_us,
                            false,
                            &spi_interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    end_us = now_us + DWM3000_RUNTIME_STATUS_POLL_PERIOD_US;
    if (end_us < spi_interval.end_us) {
        end_us = spi_interval.end_us;
    }
    runtime->cpu_busy_until_us = end_us;
    if (interval != NULL) {
        interval->start_us = now_us;
        interval->end_us = end_us;
        interval->operation = DWM3000_RUNTIME_OP_STATUS_POLL;
    }
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_read_frame(struct dwm3000_runtime *runtime,
                               size_t frame_bytes_without_fcs,
                               uint64_t now_us,
                               struct dwm3000_runtime_interval *interval)
{
    if (runtime == NULL || frame_bytes_without_fcs == 0u) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    return run_scoped_frame_spi_operation(runtime,
                                          DWM3000_RUNTIME_OP_FRAME_READ,
                                          frame_bytes_without_fcs,
                                          now_us,
                                          true,
                                          interval);
}

int dwm3000_runtime_read_cir(struct dwm3000_runtime *runtime,
                             size_t cir_bytes,
                             uint64_t now_us,
                             struct dwm3000_runtime_interval *interval)
{
    if (runtime == NULL || cir_bytes == 0u) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    return run_spi_operation(runtime,
                             DWM3000_RUNTIME_SPI_FAST,
                             DWM3000_RUNTIME_OP_CIR_READ,
                             cir_bytes + DWM3000_RUNTIME_FRAME_IO_HEADER_BYTES,
                             now_us,
                             true,
                             interval);
}

int dwm3000_runtime_process_range_frame(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval)
{
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (!runtime->awake || !runtime->configured || !runtime->pll_locked) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    if (runtime->spi_rate != DWM3000_RUNTIME_SPI_FAST) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_SPI_ORDER);
    }
    if (now_us > UINT64_MAX - DWM3000_RUNTIME_RANGE_PROCESS_WORST_US) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_OVERFLOW);
    }
    runtime->spi_busy_until_us =
        now_us + DWM3000_RUNTIME_RANGE_PROCESS_WORST_US;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_RANGE_PROCESS,
                  now_us,
                  now_us + DWM3000_RUNTIME_RANGE_PROCESS_WORST_US,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_read_rx_diagnostics(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval)
{
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (!runtime->awake || !runtime->configured || !runtime->pll_locked) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    if (runtime->spi_rate != DWM3000_RUNTIME_SPI_FAST) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_SPI_ORDER);
    }
    if (now_us > UINT64_MAX - DWM3000_RUNTIME_RX_DIAGNOSTICS_WORST_US) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_OVERFLOW);
    }
    runtime->spi_busy_until_us =
        now_us + DWM3000_RUNTIME_RX_DIAGNOSTICS_WORST_US;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_RX_DIAGNOSTICS,
                  now_us,
                  now_us + DWM3000_RUNTIME_RX_DIAGNOSTICS_WORST_US,
                  interval);
    return DWM3000_RUNTIME_OK;
}

int dwm3000_runtime_enter_retained_sleep(
    struct dwm3000_runtime *runtime,
    uint64_t now_us,
    struct dwm3000_runtime_interval *interval)
{
    int ret = begin_idle_operation(runtime, now_us);

    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    if (!runtime->awake || !runtime->configured || !runtime->pll_locked) {
        return runtime_fail(runtime, DWM3000_RUNTIME_ERR_NOT_READY);
    }
    runtime->retained_common = true;
    runtime->retained_txrx = true;
    runtime->awake = false;
    runtime->pll_locked = false;
    runtime->radio_state = DWM3000_RUNTIME_RADIO_SLEEP;
    note_interval(runtime,
                  DWM3000_RUNTIME_OP_SLEEP,
                  now_us,
                  now_us + 1u,
                  interval);
    return DWM3000_RUNTIME_OK;
}
