#include "dwm3000_driver.h"
#include "app_wake_train_politeness.h"
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
static bool radio_state_unknown, radio_configured, radio_awake;
static int pending_port_error, physical_cleanup_error, stop_error;
static unsigned int port_checks;
enum physical_state_change {
    PHYSICAL_STATE_UNCHANGED,
    PHYSICAL_STATE_UNKNOWN,
    PHYSICAL_STATE_UNCONFIGURED,
    PHYSICAL_STATE_ASLEEP,
};
static enum physical_state_change physical_state_change;
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
static int take_port_error(const char *operation)
{
    port_checks++;
    assert(strcmp(operation, port_checks == 1u ? "receive-frame-cleanup" :
                  "receive-frame-oversize-cleanup") == 0);
    assert(port_checks <= 2u);
    int ret = pending_port_error;

    pending_port_error = 0;
    if (ret < 0) {
        radio_state_unknown = true;
        radio_configured = radio_awake = false;
    }
    return ret;
}
uint32_t dwt_readsystimestamphi32(void) { return 100; }
void dwt_forcetrxoff(void) { stops++; pending_port_error = stop_error; }
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
                         uint64_t *ipatov_rx_timestamp, bool capture_rsl,
                         uint64_t absolute_deadline_ms)
{
    assert(++receives == 1); /* A failed physical attempt must reach its caller. */
    assert(timeout_ms == 50 && arms == 1);
    assert(receive_abort_enabled == (int)expect_abort_enabled);
    assert(cir_sample == NULL && cir_sampled == NULL && clock_offset_raw == NULL);
    assert(clock_offset_sampled == NULL && carrier_integrator == NULL);
    assert(carrier_integrator_sampled == NULL && ipatov_rx_timestamp == NULL);
    assert(capture_rsl);
    assert(absolute_deadline_ms == 0u);
    *status = physical_status;
    radio_state_unknown = physical_state_change == PHYSICAL_STATE_UNKNOWN;
    radio_configured = physical_state_change != PHYSICAL_STATE_UNCONFIGURED;
    radio_awake = physical_state_change != PHYSICAL_STATE_ASLEEP;
    pending_port_error = physical_cleanup_error;
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
    last_rx_host_uptime_valid = true;
    return 0;
}

#include "dwm3000_receive_error_production.inc"

struct receive_case {
    int physical_result;
    uint32_t status;
    int expected_result;
    enum dwm3000_rx_failure expected_failure;
    bool expected_activity;
    enum physical_state_change state_change;
    int cleanup_error;
    int stop_error;
    size_t frame_cap;
};

static void run_case(const struct receive_case *test, bool abortible)
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
    port_checks = 0;
    pending_port_error = 0;
    physical_result = test->physical_result;
    physical_status = test->status;
    physical_state_change = test->state_change;
    physical_cleanup_error = test->cleanup_error;
    stop_error = test->stop_error;
    radio_state_unknown = false;
    radio_configured = radio_awake = true;
    expect_abort_enabled = abortible;

    int ret = receive_frame_with_preamble_timeout(50, 16, frame,
        test->frame_cap == 0u ? sizeof(frame) : test->frame_cap,
        &length, &quality, &rsl, &failure, &timing, abortible);
    assert(ret == test->expected_result);
    assert(arms == 1 && receives == 1 && clears == 1);
    assert(stops == (unsigned int)(test->frame_cap > 0u));
    assert(port_checks == 1u + stops);
    assert(pending_port_error == 0);
    assert(receive_abort_enabled == 0);
    assert(last_rx_host_uptime_valid == (test->expected_result == 0));
    assert(failure == test->expected_failure);
    assert(app_wake_train_politeness_rx_activity(ret, failure) ==
           test->expected_activity);
    dwm3000_driver_last_rx_debug_get(&debug);
    assert(debug.status == test->status);
    assert(debug.channel == phy.chan && debug.phy_mode == active_phy_mode);
    assert(debug.sfd_timeout == phy.sfdTO && debug.rx_code == phy.rxCode);
    if (test->expected_result < 0) {
        assert(length == 0 && !timing.valid && timing.rx_timestamp == 0);
        assert(rsl == DWM3000_RSL_INVALID_DBM);
        assert(debug.rx_finfo == (stops > 0u ? 0x1234u : 0u));
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
    static const struct receive_case ordinary_cases[] = {
        { .physical_result = -EIO, .status = SYS_STATUS_RXSTO_BIT_MASK,
          .expected_result = -EIO, .expected_failure = DWM3000_RX_FAILURE_SFD_TIMEOUT,
          .expected_activity = true },
        { .physical_result = -EIO, .status = SYS_STATUS_RXPHE_BIT_MASK,
          .expected_result = -EIO, .expected_failure = DWM3000_RX_FAILURE_CRC_OR_PHY,
          .expected_activity = true },
        { .physical_result = -EIO, .status = SYS_STATUS_RXFCE_BIT_MASK,
          .expected_result = -EIO, .expected_failure = DWM3000_RX_FAILURE_CRC_OR_PHY,
          .expected_activity = true },
        { .physical_result = -ETIMEDOUT, .status = SYS_STATUS_RXFTO_BIT_MASK,
          .expected_result = -ETIMEDOUT,
          .expected_failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT,
          .expected_activity = true },
        { .physical_result = -ETIMEDOUT, .status = SYS_STATUS_RXPTO_BIT_MASK,
          .expected_result = -ETIMEDOUT,
          .expected_failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT },
        { .physical_result = 0, .status = SYS_STATUS_RXFCG_BIT_MASK,
          .expected_result = 0, .expected_failure = DWM3000_RX_FAILURE_NONE,
          .expected_activity = true },
    };
    static const uint32_t residual_statuses[] = {
        0, SYS_STATUS_RXFCG_BIT_MASK, SYS_STATUS_RXSTO_BIT_MASK, SYS_STATUS_RXPHE_BIT_MASK,
        SYS_STATUS_RXFCE_BIT_MASK, SYS_STATUS_RXFTO_BIT_MASK,
        SYS_STATUS_RXPTO_BIT_MASK,
        SYS_STATUS_RXPTO_BIT_MASK | SYS_STATUS_RXFCE_BIT_MASK,
    };
    static const int physical_errors[] = {
        -EIO, -ENODEV, -ETIMEDOUT, -ECANCELED, 0,
    };
    static const int cleanup_errors[] = { -EIO, -ENODEV, -ETIMEDOUT };

    for (unsigned int abortible = 0; abortible < 2; abortible++) {
        for (size_t i = 0; i < sizeof(ordinary_cases) / sizeof(ordinary_cases[0]); i++) {
            run_case(&ordinary_cases[i], abortible != 0);
        }
        const struct receive_case oversized = {
            .physical_result = 0, .status = SYS_STATUS_RXFCG_BIT_MASK,
            .expected_result = -EMSGSIZE,
            .expected_failure = DWM3000_RX_FAILURE_BAD_FRAME,
            .expected_activity = true, .frame_cap = sizeof(payload) - 1u,
        };
        run_case(&oversized, abortible != 0);
        for (size_t e = 0; e < sizeof(cleanup_errors) / sizeof(cleanup_errors[0]); e++) {
            const struct receive_case oversized_cleanup = {
                .physical_result = 0, .status = SYS_STATUS_RXFCG_BIT_MASK,
                .expected_result = cleanup_errors[e] == -ETIMEDOUT ?
                                   -EIO : cleanup_errors[e],
                .stop_error = cleanup_errors[e],
                .frame_cap = sizeof(payload) - 1u,
            };
            run_case(&oversized_cleanup, abortible != 0);
        }
        for (size_t s = 0; s < sizeof(residual_statuses) / sizeof(residual_statuses[0]); s++) {
            /* Cancellation cannot publish stale RF evidence either. */
            const struct receive_case canceled = {
                .physical_result = -ECANCELED, .status = residual_statuses[s],
                .expected_result = -ECANCELED,
            };
            run_case(&canceled, abortible != 0);

            /* Each state predicate independently vetoes stale RX evidence;
             * no one predicate may stand in for the complete health check. */
            for (unsigned int state = PHYSICAL_STATE_UNKNOWN;
                 state <= PHYSICAL_STATE_ASLEEP; state++) {
                for (size_t e = 0; e < sizeof(physical_errors) / sizeof(physical_errors[0]); e++) {
                    const struct receive_case fatal = {
                        .physical_result = physical_errors[e],
                        .status = residual_statuses[s],
                        .expected_result = physical_errors[e] == -ETIMEDOUT ||
                                           physical_errors[e] == -ECANCELED ||
                                           physical_errors[e] == 0 ?
                                           -EIO : physical_errors[e],
                        .state_change = (enum physical_state_change)state,
                    };
                    run_case(&fatal, abortible != 0);
                }
            }
            /* The final cleanup can fail after a completed RF timeout. Its
             * latched transport error must win before status classification. */
            for (size_t e = 0; e < sizeof(cleanup_errors) / sizeof(cleanup_errors[0]); e++) {
                for (unsigned int success = 0u; success < 2u; success++) {
                    const struct receive_case cleanup = {
                        .physical_result = success ? 0 : -ETIMEDOUT,
                        .status = residual_statuses[s],
                        .expected_result = cleanup_errors[e] == -ETIMEDOUT ?
                                           -EIO : cleanup_errors[e],
                        .cleanup_error = cleanup_errors[e],
                    };
                    run_case(&cleanup, abortible != 0);
                }
            }
        }
    }
    puts("production receive error boundary passed");
    return 0;
}
