#include "app_clicker_radio_power.h"
#include "app_mesh_radio_client.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum call_event {
    CALL_OWNER_CLAIM = 1,
    CALL_OWNER_RELEASE,
    CALL_PORT_INIT,
    CALL_PORT_WAKEUP,
    CALL_PORT_RESET,
    CALL_DRIVER_PROBE,
    CALL_PORT_FAST_SPI,
    CALL_PORT_CURRENT_SPI,
    CALL_DRIVER_STANDBY,
    CALL_DRIVER_CONFIGURE_WAKE,
    CALL_DRIVER_FORCE_RECOVERY,
    CALL_DRIVER_CONFIGURE_DEFAULT,
    CALL_PORT_PREPARE_SYSTEMOFF,
};

static enum call_event calls[32];
static size_t call_count;
static enum call_event failing_call;
static uint8_t failures_remaining;
static int failure_code;
static int owner_claim_result;
static bool owner_live;
static uint32_t next_generation;

static int note_call(enum call_event event)
{
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = event;
    if (event == failing_call && failures_remaining > 0u) {
        failures_remaining--;
        return failure_code;
    }
    return 0;
}

static void reset_scenario(void)
{
    memset(calls, 0, sizeof(calls));
    call_count = 0u;
    failing_call = 0;
    failures_remaining = 0u;
    failure_code = -EIO;
    owner_claim_result = 0;
    owner_live = false;
    next_generation = 0u;
}

static void fail_next(enum call_event event, int error)
{
    failing_call = event;
    failures_remaining = 1u;
    failure_code = error;
}

static void expect_calls(const enum call_event *expected,
                         size_t expected_count)
{
    assert(call_count == expected_count);
    for (size_t i = 0u; i < expected_count; i++) {
        assert(calls[i] == expected[i]);
    }
}

int app_mesh_radio_owner_try_claim(
    enum app_mesh_radio_client client,
    const char *reason,
    struct app_mesh_radio_owner_lease *lease_out)
{
    assert(client == APP_MESH_RADIO_CLIENT_CLICKER);
    assert(reason != NULL);
    assert(lease_out != NULL);
    (void)note_call(CALL_OWNER_CLAIM);
    if (owner_claim_result < 0) {
        return owner_claim_result;
    }
    assert(!owner_live);
    owner_live = true;
    next_generation++;
    lease_out->generation = next_generation;
    lease_out->client = client;
    return 0;
}

int app_mesh_radio_owner_release(struct app_mesh_radio_owner_lease *lease)
{
    (void)note_call(CALL_OWNER_RELEASE);
    assert(owner_live);
    assert(lease != NULL);
    assert(lease->generation == next_generation);
    assert(lease->client == APP_MESH_RADIO_CLIENT_CLICKER);
    owner_live = false;
    memset(lease, 0, sizeof(*lease));
    return 0;
}

int dwm3000_port_init(void)
{
    return note_call(CALL_PORT_INIT);
}

int dwm3000_port_wakeup(void)
{
    return note_call(CALL_PORT_WAKEUP);
}

int dwm3000_port_hw_reset(void)
{
    return note_call(CALL_PORT_RESET);
}

int dwm3000_driver_probe(uint32_t *dev_id)
{
    int ret = note_call(CALL_DRIVER_PROBE);

    assert(dev_id != NULL);
    *dev_id = 0xDECA0302u;
    return ret;
}

int dwm3000_port_set_fast_spi(void)
{
    return note_call(CALL_PORT_FAST_SPI);
}

uint32_t dwm3000_port_current_spi_hz(void)
{
    (void)note_call(CALL_PORT_CURRENT_SPI);
    return 8000000u;
}

int dwm3000_driver_standby(void)
{
    return note_call(CALL_DRIVER_STANDBY);
}

int dwm3000_driver_configure_wake_mode(void)
{
    return note_call(CALL_DRIVER_CONFIGURE_WAKE);
}

int dwm3000_driver_force_recovery(void)
{
    return note_call(CALL_DRIVER_FORCE_RECOVERY);
}

int dwm3000_driver_configure_default(void)
{
    return note_call(CALL_DRIVER_CONFIGURE_DEFAULT);
}

int dwm3000_port_prepare_systemoff(void)
{
    return note_call(CALL_PORT_PREPARE_SYSTEMOFF);
}

static void test_self_test_preflight_releases_before_diagnostic_owner(void)
{
    static const enum call_event expected[] = {
        CALL_OWNER_CLAIM,
        CALL_PORT_INIT,
        CALL_PORT_WAKEUP,
        CALL_PORT_RESET,
        CALL_DRIVER_PROBE,
        CALL_PORT_FAST_SPI,
        CALL_PORT_CURRENT_SPI,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };
    struct app_mesh_radio_owner_lease diagnostic = {0};

    reset_scenario();
    assert(app_clicker_radio_self_test_preflight() == 0);
    expect_calls(expected, sizeof(expected) / sizeof(expected[0]));
    assert(!owner_live);
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "diagnostic follows preflight",
               &diagnostic) == 0);
    assert(app_mesh_radio_owner_release(&diagnostic) == 0);
}

static void test_self_test_failure_and_busy_paths_never_bypass_owner(void)
{
    static const enum call_event failed[] = {
        CALL_OWNER_CLAIM,
        CALL_PORT_INIT,
        CALL_PORT_WAKEUP,
        CALL_PORT_RESET,
        CALL_DRIVER_PROBE,
        CALL_OWNER_RELEASE,
    };
    static const enum call_event busy[] = {
        CALL_OWNER_CLAIM,
    };

    reset_scenario();
    fail_next(CALL_DRIVER_PROBE, -ENODEV);
    assert(app_clicker_radio_self_test_preflight() == -ENODEV);
    expect_calls(failed, sizeof(failed) / sizeof(failed[0]));
    assert(!owner_live);

    reset_scenario();
    owner_claim_result = -EBUSY;
    assert(app_clicker_radio_self_test_preflight() == -EBUSY);
    expect_calls(busy, sizeof(busy) / sizeof(busy[0]));
}

static void test_self_test_propagates_standby_failure_before_release(void)
{
    static const enum call_event expected[] = {
        CALL_OWNER_CLAIM,
        CALL_PORT_INIT,
        CALL_PORT_WAKEUP,
        CALL_PORT_RESET,
        CALL_DRIVER_PROBE,
        CALL_PORT_FAST_SPI,
        CALL_PORT_CURRENT_SPI,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };

    reset_scenario();
    fail_next(CALL_DRIVER_STANDBY, -EIO);
    assert(app_clicker_radio_self_test_preflight() == -EIO);
    expect_calls(expected, sizeof(expected) / sizeof(expected[0]));
    assert(!owner_live);
}

static void test_scheduled_burst_finish_parks_live_lease_while_abort_pending(void)
{
    static const enum call_event expected[] = {
        CALL_OWNER_CLAIM,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };
    struct app_mesh_radio_owner_lease lease = {0};

    reset_scenario();
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "scheduled burst",
               &lease) == 0);
    owner_claim_result = -ECANCELED;
    assert(app_clicker_radio_finish_scheduled_burst(&lease) == 0);
    expect_calls(expected, sizeof(expected) / sizeof(expected[0]));
    assert(!owner_live);
    assert(lease.generation == 0u);
}

static void test_scheduled_burst_finish_parks_live_lease_while_pause_pending(void)
{
    static const enum call_event expected[] = {
        CALL_OWNER_CLAIM,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };
    struct app_mesh_radio_owner_lease lease = {0};

    reset_scenario();
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "scheduled burst",
               &lease) == 0);
    owner_claim_result = -ESHUTDOWN;
    assert(app_clicker_radio_finish_scheduled_burst(&lease) == 0);
    expect_calls(expected, sizeof(expected) / sizeof(expected[0]));
    assert(!owner_live);
    assert(lease.generation == 0u);
}

static void test_scheduled_burst_finish_releases_after_standby_failure(void)
{
    static const enum call_event expected[] = {
        CALL_OWNER_CLAIM,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };
    struct app_mesh_radio_owner_lease lease = {0};

    reset_scenario();
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "scheduled burst",
               &lease) == 0);
    fail_next(CALL_DRIVER_STANDBY, -EIO);
    assert(app_clicker_radio_finish_scheduled_burst(&lease) == -EIO);
    expect_calls(expected, sizeof(expected) / sizeof(expected[0]));
    assert(!owner_live);
    assert(lease.generation == 0u);
}

static void test_retained_standby_owns_recovery_and_bounded_retry(void)
{
    static const enum call_event recovered[] = {
        CALL_OWNER_CLAIM,
        CALL_DRIVER_CONFIGURE_WAKE,
        CALL_DRIVER_FORCE_RECOVERY,
        CALL_DRIVER_CONFIGURE_WAKE,
        CALL_DRIVER_STANDBY,
        CALL_OWNER_RELEASE,
    };
    static const enum call_event busy[] = {
        CALL_OWNER_CLAIM,
    };

    reset_scenario();
    fail_next(CALL_DRIVER_CONFIGURE_WAKE, -EIO);
    assert(app_clicker_radio_enter_retained_standby() == 0);
    expect_calls(recovered, sizeof(recovered) / sizeof(recovered[0]));
    assert(!owner_live);

    reset_scenario();
    owner_claim_result = -EBUSY;
    assert(app_clicker_radio_enter_retained_standby() == -EBUSY);
    expect_calls(busy, sizeof(busy) / sizeof(busy[0]));
}

static void test_systemoff_parks_pins_and_keeps_terminal_lease(void)
{
    static const enum call_event success[] = {
        CALL_OWNER_CLAIM,
        CALL_PORT_INIT,
        CALL_PORT_WAKEUP,
        CALL_PORT_RESET,
        CALL_DRIVER_PROBE,
        CALL_DRIVER_CONFIGURE_DEFAULT,
        CALL_DRIVER_STANDBY,
        CALL_PORT_PREPARE_SYSTEMOFF,
    };
    static const enum call_event failed[] = {
        CALL_OWNER_CLAIM,
        CALL_PORT_INIT,
        CALL_PORT_PREPARE_SYSTEMOFF,
    };
    static const enum call_event busy[] = {
        CALL_OWNER_CLAIM,
    };
    struct app_mesh_radio_owner_lease terminal = {0};

    reset_scenario();
    assert(app_clicker_radio_prepare_systemoff(&terminal) == 0);
    expect_calls(success, sizeof(success) / sizeof(success[0]));
    assert(owner_live);
    assert(terminal.generation != 0u);
    assert(terminal.client == APP_MESH_RADIO_CLIENT_CLICKER);

    reset_scenario();
    fail_next(CALL_PORT_INIT, -EIO);
    memset(&terminal, 0, sizeof(terminal));
    assert(app_clicker_radio_prepare_systemoff(&terminal) == -EIO);
    expect_calls(failed, sizeof(failed) / sizeof(failed[0]));
    assert(owner_live);
    assert(terminal.generation != 0u);

    reset_scenario();
    owner_claim_result = -EBUSY;
    memset(&terminal, 0, sizeof(terminal));
    assert(app_clicker_radio_prepare_systemoff(&terminal) == -EBUSY);
    expect_calls(busy, sizeof(busy) / sizeof(busy[0]));
    assert(!owner_live);
    assert(terminal.generation == 0u);

    reset_scenario();
    assert(app_clicker_radio_prepare_systemoff(NULL) == -EINVAL);
    assert(call_count == 0u);
}

int main(void)
{
    test_self_test_preflight_releases_before_diagnostic_owner();
    test_self_test_failure_and_busy_paths_never_bypass_owner();
    test_self_test_propagates_standby_failure_before_release();
    test_scheduled_burst_finish_parks_live_lease_while_abort_pending();
    test_scheduled_burst_finish_parks_live_lease_while_pause_pending();
    test_scheduled_burst_finish_releases_after_standby_failure();
    test_retained_standby_owns_recovery_and_bounded_retry();
    test_systemoff_parks_pins_and_keeps_terminal_lease();
    return 0;
}
