/* Execute the production reset initializer and IDLE_RC wait. Only the port,
 * SDK and clock boundaries are faked; slow SPI consumes elapsed time. */
#include "deca_device_api.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CYCLES_PER_US 64u
#define SLOW_SPI_HZ 2000000u
#define FAST_SPI_HZ 32000000u
#define DWM3000_TRANSITION_TRACE(...) trace(__VA_ARGS__)

enum { DWM3000_PHY_NONE, DWM3000_PHY_WAKE };
static bool radio_configured, radio_awake, radio_state_unknown;
static unsigned active_phy_mode;
static uint64_t elapsed_us, ready_at_us;
static uint32_t cycles, spi_hz, read_cost_us, deadline_us;
static uint32_t poll_pause_us, busy_wait_pause_us;
static unsigned poll_pause_at, fail_poll_at;
static unsigned ports, slow_changes, polls, error_checks, sdk_calls;
static unsigned fatal_checks, identity_checks, fast_changes, awake_marks;
static uint64_t sdk_at_us;
static uint64_t readiness_window_start_us, soft_reset_at_us;
static uint64_t soft_reset_ready_delay_us;
static uint32_t soft_reset_cost_us, soft_reset_poll_cost_us;
static unsigned soft_reset_calls, first_window_polls;
static int port_error, injected_poll_error, sdk_result, sdk_port_error;
static int soft_reset_error, soft_reset_poll_error;
static int fatal_error, identity_error, fast_error;
static bool latest_ready_sample, readiness_confirmed, local_data_ready;
static int observed_sdk_mode;

static void advance_us(uint32_t amount)
{
    elapsed_us += amount;
    cycles += amount * CYCLES_PER_US;
}
static uint32_t k_cycle_get_32(void) { return cycles; }
static uint32_t k_cyc_to_us_floor32(uint32_t value) { return value / CYCLES_PER_US; }
static void k_busy_wait(uint32_t amount)
{
    advance_us(amount + busy_wait_pause_us);
    busy_wait_pause_us = 0u;
}
static void *k_current_get(void) { return NULL; }
static uint32_t radio_transition_trace_next(void) { return 1u; }
static void trace(const char *format, ...) { (void)format; }
static uint32_t dwm3000_port_current_spi_hz(void) { return spi_hz; }
static int dwm3000_port_init(void)
{
    ports++;
    readiness_confirmed = false;
    return 0;
}
static int ensure_local_data(void)
{
    local_data_ready = true; /* SDK's host pointer only. */
    return 0;
}
static void dwm3000_port_clear_error(void) { port_error = 0; }
static int dwm3000_port_set_slow_spi(void)
{
    assert(ports == 1u && sdk_calls == 0u);
    slow_changes++;
    spi_hz = SLOW_SPI_HZ;
    return 0;
}
static void invalidate_radio_state_tagged(const char *tag)
{
    (void)tag;
    radio_configured = radio_awake = false;
    radio_state_unknown = true;
    active_phy_mode = DWM3000_PHY_NONE;
}
uint8_t dwt_checkidlerc(void)
{
    assert(spi_hz == SLOW_SPI_HZ && sdk_calls == 0u && fast_changes == 0u);
    polls++;
    if (polls == poll_pause_at) advance_us(poll_pause_us);
    advance_us(read_cost_us);
    if (polls == fail_poll_at) port_error = injected_poll_error;
    latest_ready_sample = elapsed_us >= ready_at_us;
    return latest_ready_sample ? 1u : 0u;
}
static int take_port_error(const char *operation)
{
    (void)operation;
    assert(spi_hz == SLOW_SPI_HZ);
    error_checks++;
    int ret = port_error;
    port_error = 0;
    if (ret < 0) invalidate_radio_state_tagged(operation);
    if (sdk_calls == 0u) readiness_confirmed = latest_ready_sample && ret == 0;
    return ret;
}
void dwt_softreset(void)
{
    assert(local_data_ready && spi_hz == SLOW_SPI_HZ);
    assert(sdk_calls == 0u && fast_changes == 0u && port_error == 0);
    assert(elapsed_us >= deadline_us && soft_reset_calls == 0u);
    soft_reset_calls++;
    soft_reset_at_us = elapsed_us;
    first_window_polls = polls;
    advance_us(soft_reset_cost_us); /* SDK sleeps and SPI transfers are nonzero. */
    latest_ready_sample = readiness_confirmed = false;
    readiness_window_start_us = elapsed_us;
    ready_at_us = soft_reset_ready_delay_us == UINT64_MAX ? UINT64_MAX :
                  elapsed_us + soft_reset_ready_delay_us;
    port_error = soft_reset_error;
    if (soft_reset_poll_cost_us != 0u) read_cost_us = soft_reset_poll_cost_us;
    if (soft_reset_poll_error != 0) {
        fail_poll_at = polls + 1u;
        injected_poll_error = soft_reset_poll_error;
    }
}
int dwt_initialise(int mode)
{
    /* These assertions make removal/reordering of the production wait fail
     * at the forbidden SDK access, including a ready-looking SPI failure. */
    assert(spi_hz == SLOW_SPI_HZ && readiness_confirmed && polls > 0u);
    assert(elapsed_us - readiness_window_start_us < deadline_us);
    assert(error_checks == polls + soft_reset_calls);
    assert(soft_reset_calls == 0u || polls > first_window_polls);
    sdk_calls++;
    sdk_at_us = elapsed_us;
    observed_sdk_mode = mode;
    port_error = sdk_port_error;
    return sdk_result;
}
static int check_device_fatal_status(const char *operation)
{
    (void)operation;
    assert(spi_hz == SLOW_SPI_HZ && readiness_confirmed && sdk_calls == 1u);
    fatal_checks++;
    return fatal_error;
}
static int validate_device_identity(const char *operation)
{
    (void)operation;
    assert(spi_hz == SLOW_SPI_HZ && readiness_confirmed && fatal_checks == 1u);
    identity_checks++;
    return identity_error;
}
static int dwm3000_port_set_fast_spi(void)
{
    assert(readiness_confirmed && sdk_calls == 1u && fatal_checks == 1u);
    assert(identity_checks == 1u);
    fast_changes++;
    if (fast_error != 0) return fast_error;
    spi_hz = FAST_SPI_HZ;
    return 0;
}
static void mark_radio_awake_unconfigured_tagged(const char *tag)
{
    (void)tag;
    assert(spi_hz == FAST_SPI_HZ && fast_changes == 1u);
    radio_awake = true;
    radio_configured = radio_state_unknown = false;
    active_phy_mode = DWM3000_PHY_NONE;
    awake_marks++;
}

/* CMake prepends the actual driver timing constants, so this fixture does not
 * silently use a shorter timeout than the production reset/wake path. */
#include "dwm3000_reset_readiness_production.inc"

static void fixture(uint32_t cycle_start)
{
    cycles = cycle_start;
    elapsed_us = 0u;
    ready_at_us = 0u;
    deadline_us = DWM3000_WAKE_IDLE_RC_TIMEOUT_US;
    read_cost_us = 24u; /* Six command/data bytes at 2 MHz. */
    spi_hz = FAST_SPI_HZ;
    ports = slow_changes = polls = error_checks = sdk_calls = 0u;
    fatal_checks = identity_checks = fast_changes = awake_marks = 0u;
    fail_poll_at = poll_pause_at = poll_pause_us = busy_wait_pause_us = 0u;
    sdk_at_us = 0u;
    readiness_window_start_us = soft_reset_at_us = 0u;
    soft_reset_ready_delay_us = UINT64_MAX;
    soft_reset_cost_us = 2120u;
    soft_reset_poll_cost_us = 0u;
    soft_reset_calls = first_window_polls = 0u;
    soft_reset_error = soft_reset_poll_error = 0;
    port_error = sdk_port_error = fatal_error = identity_error = fast_error = 0;
    sdk_result = DWT_SUCCESS;
    injected_poll_error = -EIO;
    latest_ready_sample = readiness_confirmed = local_data_ready = false;
    radio_configured = radio_awake = true;
    radio_state_unknown = false;
    active_phy_mode = DWM3000_PHY_WAKE;
}
static void assert_failed_before_sdk(void)
{
    assert(sdk_calls == 0u && fast_changes == 0u && awake_marks == 0u);
    assert(spi_hz == SLOW_SPI_HZ && error_checks == polls + soft_reset_calls);
    assert(!radio_awake && !radio_configured && radio_state_unknown);
    assert(active_phy_mode == DWM3000_PHY_NONE);
}
static void test_ready_proof_precedes_sdk_and_fast_spi(uint32_t cycle_start)
{
    const uint32_t delays[] = {0u, 500u, 1500u};
    for (unsigned mode = 0u; mode < 2u; mode++) {
        for (unsigned i = 0u; i < sizeof(delays) / sizeof(delays[0]); i++) {
            fixture(cycle_start);
            ready_at_us = delays[i];
            assert(initialise_radio(mode != 0u) == 0);
            assert(sdk_calls == 1u && fast_changes == 1u && awake_marks == 1u);
            assert(soft_reset_calls == 0u);
            assert(sdk_at_us >= delays[i] && sdk_at_us < deadline_us);
            assert(observed_sdk_mode == (mode ? DWT_DW_IDLE : DWT_DW_INIT));
            assert(spi_hz == FAST_SPI_HZ && radio_awake && !radio_configured);
            assert(!radio_state_unknown && active_phy_mode == DWM3000_PHY_NONE);
            if (delays[i] != 0u) assert(polls > 1u);
        }
    }
}
static void test_never_ready_has_elapsed_bound(uint32_t cycle_start)
{
    const uint32_t costs[] = {24u, 200u};
    for (unsigned i = 0u; i < sizeof(costs) / sizeof(costs[0]); i++) {
        fixture(cycle_start);
        read_cost_us = costs[i];
        ready_at_us = UINT64_MAX;
        assert(initialise_radio(false) == -ETIMEDOUT);
        assert_failed_before_sdk();
        assert(soft_reset_calls == 1u);
        assert(soft_reset_at_us >= deadline_us && soft_reset_at_us < deadline_us + read_cost_us);
        assert(elapsed_us >= 2u * deadline_us + soft_reset_cost_us);
        assert(elapsed_us < 2u * (deadline_us + read_cost_us) + soft_reset_cost_us);
        assert(polls <= 2u * (deadline_us / (read_cost_us + DWM3000_STATUS_POLL_INTERVAL_US) + 1u));
    }
}
static void test_late_ready_and_scheduler_pause_cannot_initialize(void)
{
    fixture(0u);
    ready_at_us = deadline_us;
    read_cost_us = deadline_us;
    assert(initialise_radio(false) == -ETIMEDOUT);
    assert_failed_before_sdk();
    assert(soft_reset_calls == 1u && first_window_polls == 1u);

    fixture(0u);
    poll_pause_at = 1u;
    poll_pause_us = deadline_us;
    assert(initialise_radio(false) == -ETIMEDOUT);
    assert_failed_before_sdk();
    assert(soft_reset_calls == 1u && first_window_polls == 1u);

    fixture(0u);
    ready_at_us = 500u;
    busy_wait_pause_us = deadline_us;
    assert(initialise_radio(false) == -ETIMEDOUT);
    assert_failed_before_sdk();
    /* No poll after resuming beyond the first deadline; reset earns one new wait. */
    assert(soft_reset_calls == 1u && first_window_polls == 1u);
}
static void test_spi_errors_win_over_ready_and_timeout(void)
{
    for (unsigned ready = 0u; ready < 2u; ready++) {
        for (unsigned late = 0u; late < 2u; late++) {
            fixture(0u);
            ready_at_us = ready ? 0u : UINT64_MAX;
            fail_poll_at = 1u;
            if (late) read_cost_us = deadline_us;
            assert(initialise_radio(false) == -EIO);
            assert_failed_before_sdk();
            assert(polls == 1u && soft_reset_calls == 0u);
        }
    }
    fixture(0u);
    ready_at_us = UINT64_MAX;
    fail_poll_at = 3u;
    injected_poll_error = -EHOSTDOWN;
    assert(initialise_radio(false) == -EHOSTDOWN);
    assert_failed_before_sdk();
    assert(polls == 3u && soft_reset_calls == 0u);

    fixture(0u);
    fail_poll_at = 1u;
    injected_poll_error = -ETIMEDOUT; /* A bus timeout is not radio readiness expiry. */
    assert(initialise_radio(false) == -ETIMEDOUT);
    assert_failed_before_sdk();
    assert(soft_reset_calls == 0u);
}

static void test_soft_reset_can_recover_only_with_fresh_readiness(uint32_t cycle_start)
{
    const uint32_t delays[] = {0u, 500u, 1500u};
    for (unsigned i = 0u; i < sizeof(delays) / sizeof(delays[0]); i++) {
        fixture(cycle_start);
        ready_at_us = UINT64_MAX;
        soft_reset_ready_delay_us = delays[i];
        assert(initialise_radio(false) == 0);
        assert(soft_reset_calls == 1u && sdk_calls == 1u && awake_marks == 1u);
        assert(sdk_at_us >= readiness_window_start_us + delays[i]);
        assert(sdk_at_us < readiness_window_start_us + deadline_us);
        assert(polls > first_window_polls && fast_changes == 1u);
    }
    for (unsigned failure = 0u; failure < 4u; failure++) {
        fixture(cycle_start);
        ready_at_us = UINT64_MAX;
        soft_reset_ready_delay_us = 0u;
        if (failure == 0u) soft_reset_error = -EIO;
        if (failure == 1u) soft_reset_error = -ETIMEDOUT;
        if (failure == 2u) soft_reset_poll_error = -EHOSTDOWN;
        if (failure == 3u) soft_reset_poll_cost_us = deadline_us;
        const int errors[] = {-EIO, -ETIMEDOUT, -EHOSTDOWN, -ETIMEDOUT};
        assert(initialise_radio(false) == errors[failure]);
        assert_failed_before_sdk();
        assert(soft_reset_calls == 1u);
        if (failure < 2u) assert(polls == first_window_polls);
        else assert(polls == first_window_polls + 1u);
    }
}
static void test_post_ready_failures_never_publish_awake(void)
{
    for (unsigned failure = 0u; failure < 5u; failure++) {
        fixture(0u);
        if (failure == 0u) sdk_result = DWT_ERROR;
        if (failure == 1u) sdk_port_error = -EIO;
        if (failure == 2u) fatal_error = -EHOSTDOWN;
        if (failure == 3u) identity_error = -ENODEV;
        if (failure == 4u) fast_error = -EIO;
        const int errors[] = {-EIO, -EIO, -EHOSTDOWN, -ENODEV, -EIO};
        assert(initialise_radio(false) == errors[failure]);
        assert(sdk_calls == 1u && awake_marks == 0u);
        assert(fast_changes == (failure == 4u ? 1u : 0u));
    }
}
int main(void)
{
    const uint32_t starts[] = {0u, UINT32_MAX - 255u};
    for (unsigned i = 0u; i < sizeof(starts) / sizeof(starts[0]); i++) {
        test_ready_proof_precedes_sdk_and_fast_spi(starts[i]);
        test_never_ready_has_elapsed_bound(starts[i]);
        test_soft_reset_can_recover_only_with_fresh_readiness(starts[i]);
    }
    test_late_ready_and_scheduler_pause_cannot_initialize();
    test_spi_errors_win_over_ready_and_timeout();
    test_post_ready_failures_never_publish_awake();
    puts("DW3000 reset: slow-SPI IDLE_RC proof gates SDK initialization and fast SPI");
    return 0;
}
