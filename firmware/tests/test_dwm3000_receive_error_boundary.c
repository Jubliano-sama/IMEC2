#include "dwm3000_driver.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

_Static_assert(FCS_LEN == UWB_PHY_FCS_LEN, "hardware and protocol FCS agree");
#define DWM3000_RX_ERROR_STATUS_MASK \
    (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_RXOVRR_BIT_MASK)

static bool last_rx_host_uptime_valid;
static uint32_t last_rx_finfo_register;
static struct dwm3000_rx_debug_snapshot last_rx_debug;
static int receive_abort_enabled;
static int active_phy_mode = 2;
static unsigned int arms, receives, clears, stops;
static int physical_result;
static uint32_t physical_status;
static bool expect_abort_enabled;
static const uint8_t payload[] = { 0x12, 0x34, 0x56, 0x78 };
static const dwt_config_t phy = {
    .chan = 5, .txPreambLength = 8, .rxPAC = 1, .txCode = 9,
    .rxCode = 9, .sfdType = 1, .dataRate = 1, .phrMode = 1,
    .phrRate = 0, .sfdTO = 129,
};

static void atomic_set(int *value, int next) { *value = next; }
static int ensure_current_phy_or_range(void) { return 0; }
void dwt_setpreambledetecttimeout(uint16_t timeout) { assert(timeout == 16); }
void dwt_setrxtimeout(uint32_t timeout) { assert(timeout == 0); }
static void clear_all_events(void) { clears++; }
static int start_immediate_rx(void) { arms++; return 0; }
uint32_t dwt_readsystimestamphi32(void) { return 100; }
void dwt_forcetrxoff(void) { stops++; }
static const dwt_config_t *config_for_phy(int mode)
{
    assert(mode == active_phy_mode);
    return &phy;
}
static uint16_t effective_sfd_timeout(const dwt_config_t *config)
{
    return config->sfdTO;
}
static size_t payload_len_without_fcs(size_t length) { return length - FCS_LEN; }
static uint32_t dwt_time32_delta_to_uus(uint32_t start, uint32_t end)
{
    assert(start == 100 && end == 356);
    return 1;
}

/* Physical completion is the mock boundary. The wrapper and its status
 * classifier below are extracted unchanged from the production driver. */
static int receive_frame(uint32_t timeout_ms, uint32_t *status,
                         uint8_t *buffer, size_t buffer_len, size_t *frame_len,
                         uint64_t *rx_timestamp, uint8_t *quality,
                         int8_t *rsl_dbm, uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                         bool *cir_sampled, int16_t *clock_offset_raw,
                         bool *clock_offset_sampled, int32_t *carrier_integrator,
                         bool *carrier_integrator_sampled,
                         uint64_t *ipatov_rx_timestamp, bool capture_rsl)
{
    assert(++receives == 1); /* A failed physical attempt must reach its caller. */
    assert(timeout_ms == 50 && arms == 1);
    assert(receive_abort_enabled == (int)expect_abort_enabled);
    assert(cir_sample == NULL && cir_sampled == NULL && clock_offset_raw == NULL);
    assert(clock_offset_sampled == NULL && carrier_integrator == NULL);
    assert(carrier_integrator_sampled == NULL && ipatov_rx_timestamp == NULL);
    assert(capture_rsl);
    *status = physical_status;
    if (physical_result < 0) {
        return physical_result;
    }
    assert(buffer_len >= sizeof(payload) + FCS_LEN);
    memcpy(buffer, payload, sizeof(payload));
    memset(buffer + sizeof(payload), 0xee, FCS_LEN);
    *frame_len = sizeof(payload) + FCS_LEN;
    *quality = 73;
    *rsl_dbm = -64;
    if (rx_timestamp != NULL) {
        *rx_timestamp = UINT64_C(356) << 8;
    }
    last_rx_finfo_register = 0x1234;
    return 0;
}

#include "dwm3000_receive_error_production.inc"

static void run_case(int result, uint32_t status,
                     enum dwm3000_rx_failure expected_failure, bool abortible)
{
    uint8_t frame[16];
    size_t length = 99;
    uint8_t quality = 0;
    int8_t rsl = 0;
    enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_BAD_FRAME;
    struct dwm3000_rx_frame_timing timing;
    struct dwm3000_rx_debug_snapshot debug;
    memset(frame, 0xaa, sizeof(frame));
    memset(&timing, 0xff, sizeof(timing));
    memset(&last_rx_debug, 0xff, sizeof(last_rx_debug));
    last_rx_host_uptime_valid = true;
    last_rx_finfo_register = 0xdead;
    arms = receives = clears = stops = 0;
    receive_abort_enabled = 0;
    physical_result = result;
    physical_status = status;
    expect_abort_enabled = abortible;

    int ret = receive_frame_with_preamble_timeout(50, 16, frame, sizeof(frame),
        &length, &quality, &rsl, &failure, &timing, abortible);
    assert(ret == result);
    assert(arms == 1 && receives == 1 && clears == 1 && stops == 0);
    assert(receive_abort_enabled == 0 && !last_rx_host_uptime_valid);
    assert(failure == expected_failure);
    dwm3000_driver_last_rx_debug_get(&debug);
    assert(debug.status == status);
    assert(debug.channel == phy.chan && debug.phy_mode == active_phy_mode);
    assert(debug.sfd_timeout == phy.sfdTO && debug.rx_code == phy.rxCode);
    if (result < 0) {
        assert(length == 0 && !timing.valid && timing.rx_timestamp == 0);
        assert(rsl == DWM3000_RSL_INVALID_DBM && debug.rx_finfo == 0);
        for (size_t i = 0; i < sizeof(frame); i++) {
            assert(frame[i] == 0xaa);
        }
    } else {
        assert(length == sizeof(payload) && memcmp(frame, payload, length) == 0);
        assert(frame[length] == 0xaa); /* FCS is not exposed to the caller. */
        assert(quality == 73 && rsl == -64 && debug.rx_finfo == 0x1234);
        assert(timing.valid && timing.rx_timestamp == (UINT64_C(356) << 8));
        assert(timing.rx_enable_time32 == 100 && timing.rx_timestamp_time32 == 356);
        assert(timing.rx_since_enable_uus == 1);
    }
}

int main(void)
{
    for (unsigned int abortible = 0; abortible < 2; abortible++) {
        run_case(-EIO, SYS_STATUS_RXSTO_BIT_MASK,
                 DWM3000_RX_FAILURE_SFD_TIMEOUT, abortible != 0);
        run_case(-EIO, SYS_STATUS_RXPHE_BIT_MASK,
                 DWM3000_RX_FAILURE_CRC_OR_PHY, abortible != 0);
        run_case(-EIO, SYS_STATUS_RXFCE_BIT_MASK,
                 DWM3000_RX_FAILURE_CRC_OR_PHY, abortible != 0);
        run_case(-ETIMEDOUT, SYS_STATUS_RXPTO_BIT_MASK,
                 DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT, abortible != 0);
        run_case(0, SYS_STATUS_RXFCG_BIT_MASK, DWM3000_RX_FAILURE_NONE, abortible != 0);
    }
    run_case(-ECANCELED, 0, DWM3000_RX_FAILURE_NONE, true);
    puts("production receive error boundary passed");
    return 0;
}
