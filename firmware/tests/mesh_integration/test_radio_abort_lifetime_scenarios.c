#include "app_mesh_radio_client.h"
#include "app_mesh_radio_owner.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

static bool driver_abort_asserted;
static uint32_t driver_abort_requests;
static uint32_t driver_abort_clears;
static uint32_t quiet_enters;
static uint32_t quiet_exits;

static void enter_uwb_quiet(const char *reason)
{
    assert(reason != NULL);
    quiet_enters++;
}

static void exit_uwb_quiet(const char *reason)
{
    assert(reason != NULL);
    quiet_exits++;
}

static void request_receive_abort(void)
{
    driver_abort_asserted = true;
    driver_abort_requests++;
}

static void clear_receive_abort(void)
{
    driver_abort_asserted = false;
    driver_abort_clears++;
}

int64_t k_uptime_get(void)
{
    return 0;
}

static int mock_receive_poll(void)
{
    return driver_abort_asserted ? -ECANCELED : -ETIMEDOUT;
}

static void reset_owner(void)
{
    const struct app_mesh_radio_owner_platform_ops ops = {
        .enter_uwb_quiet = enter_uwb_quiet,
        .exit_uwb_quiet = exit_uwb_quiet,
        .request_receive_abort = request_receive_abort,
        .clear_receive_abort = clear_receive_abort,
    };

    driver_abort_asserted = true;
    driver_abort_requests = 0u;
    driver_abort_clears = 0u;
    quiet_enters = 0u;
    quiet_exits = 0u;
    assert(app_mesh_radio_owner_init(&ops) == 0);
    assert(!driver_abort_asserted);
    assert(driver_abort_requests == 0u);
    assert(driver_abort_clears == 1u);
    driver_abort_clears = 0u;
}

static void test_abort_level_survives_repeated_receive_observation(void)
{
    struct app_mesh_radio_owner_abort_lease lease = {0};

    reset_owner();
    assert(app_mesh_radio_owner_abort_request(
               APP_MESH_RADIO_ABORT_HOST_COMMAND, &lease) == 0);
    assert(driver_abort_asserted);
    assert(app_mesh_radio_owner_abort_pending());
    assert(mock_receive_poll() == -ECANCELED);
    assert(mock_receive_poll() == -ECANCELED);
    assert(driver_abort_requests == 1u);
    assert(driver_abort_clears == 0u);

    assert(app_mesh_radio_owner_abort_release(&lease) == 0);
    assert(!driver_abort_asserted);
    assert(!app_mesh_radio_owner_abort_pending());
    assert(mock_receive_poll() == -ETIMEDOUT);
    assert(driver_abort_clears == 1u);
}

static void test_maintenance_clients_need_exact_non_nested_leases(void)
{
    struct app_mesh_radio_owner_lease high_debug = {0};
    struct app_mesh_radio_owner_lease forged;
    struct app_mesh_radio_owner_lease stale;
    struct app_mesh_radio_owner_lease clicker_preflight = {0};
    struct app_mesh_radio_owner_lease clicker_diagnostic = {0};

    reset_owner();
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_HIGH_DEBUG,
               "high-debug stage-0 test",
               &high_debug) == 0);
    assert(app_mesh_radio_owner_busy());
    assert(quiet_enters == 1u);
    assert(quiet_exits == 0u);

    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "clicker self-test preflight",
               &clicker_preflight) == -EBUSY);
    assert(clicker_preflight.generation == 0u);

    forged = high_debug;
    forged.client = APP_MESH_RADIO_CLIENT_CLICKER;
    assert(app_mesh_radio_owner_release(&forged) == -ESTALE);
    assert(app_mesh_radio_owner_busy());
    assert(quiet_exits == 0u);

    stale = high_debug;
    assert(app_mesh_radio_owner_release(&high_debug) == 0);
    assert(high_debug.generation == 0u);
    assert(!app_mesh_radio_owner_busy());
    assert(quiet_exits == 1u);
    assert(app_mesh_radio_owner_release(&stale) == -ESTALE);
    assert(quiet_exits == 1u);

    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "clicker self-test preflight",
               &clicker_preflight) == 0);
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "clicker diagnostic sub-operation",
               &clicker_diagnostic) == -EBUSY);
    assert(app_mesh_radio_owner_release(&clicker_preflight) == 0);
    assert(app_mesh_radio_owner_try_claim(
               APP_MESH_RADIO_CLIENT_CLICKER,
               "clicker diagnostic sub-operation",
               &clicker_diagnostic) == 0);
    assert(app_mesh_radio_owner_release(&clicker_diagnostic) == 0);
    assert(!app_mesh_radio_owner_busy());
    assert(quiet_enters == 3u);
    assert(quiet_exits == 3u);
}

static void test_only_final_exact_lease_clears_abort_level(void)
{
    struct app_mesh_radio_owner_abort_lease first = {0};
    struct app_mesh_radio_owner_abort_lease second = {0};
    struct app_mesh_radio_owner_abort_lease stale;

    reset_owner();
    assert(app_mesh_radio_owner_abort_request(
               APP_MESH_RADIO_ABORT_HOST_COMMAND, &first) == 0);
    assert(app_mesh_radio_owner_abort_request(
               APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY, &second) == 0);
    stale = first;
    assert(first.token != second.token);
    assert(driver_abort_asserted);
    assert(driver_abort_requests == 2u);

    assert(app_mesh_radio_owner_abort_release(&first) == 0);
    assert(first.token == 0u);
    assert(driver_abort_asserted);
    assert(app_mesh_radio_owner_abort_pending());
    assert(mock_receive_poll() == -ECANCELED);
    assert(driver_abort_requests == 3u);
    assert(driver_abort_clears == 0u);

    assert(app_mesh_radio_owner_abort_release(&stale) == -ESTALE);
    assert(driver_abort_asserted);
    assert(app_mesh_radio_owner_abort_pending());
    assert(driver_abort_requests == 3u);
    assert(driver_abort_clears == 0u);

    assert(app_mesh_radio_owner_abort_release(&second) == 0);
    assert(second.token == 0u);
    assert(!driver_abort_asserted);
    assert(!app_mesh_radio_owner_abort_pending());
    assert(driver_abort_clears == 1u);
}

int main(void)
{
    test_maintenance_clients_need_exact_non_nested_leases();
    test_abort_level_survives_repeated_receive_observation();
    test_only_final_exact_lease_clears_abort_level();
    return 0;
}
