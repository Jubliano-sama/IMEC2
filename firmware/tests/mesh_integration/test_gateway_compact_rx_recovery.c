#include "app_mesh_rx_policy.h"
#include "enumeration_response_lane.h"
#include "protocol.h"
#include "uwb.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The complete production collection function is extracted by CMake. Only
 * the clock, radio, lease and gateway ingress boundaries are replaced here. */
#define ROLE_GATEWAY 3
#define DEVICE_ROLE ROLE_GATEWAY
#define UWB_CONTROL_TX_TIMEOUT_MS 40u
struct radio_guard_uwb_lease { bool held; };
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static bool mesh_uwb_rx_active;
static uint64_t now_ms;
static uint64_t deadline_ms;
static unsigned rx_calls, configure_calls, recovery_calls, parked, released;
static unsigned watchdog_feeds, safe_boundaries, scheduled, published;
static int injected_ret, recovery_ret, parking_ret, release_ret;
static enum dwm3000_rx_failure injected_failure;
static bool finish_on_error;

static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }
static void status_debug_printf(const char *format, ...)
{
    (void)format;
}
static bool app_gateway_enumeration_response_window(uint64_t now,
                                                    uint64_t *deadline)
{
    *deadline = deadline_ms;
    return now < deadline_ms;
}
static int mesh_rx_radio_claim(const char *reason,
                              struct radio_guard_uwb_lease *lease)
{
    (void)reason;
    assert(!lease->held);
    lease->held = true;
    return 0;
}
int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    configure_calls++;
    return 0;
}
int dwm3000_driver_force_recovery(void)
{
    recovery_calls++;
    return recovery_ret;
}
int dwm3000_driver_receive_frame_continuous(
    uint32_t timeout_ms, uint8_t *frame, size_t frame_cap, size_t *frame_len,
    uint8_t *quality, int8_t *rsl_dbm, enum dwm3000_rx_failure *failure)
{
    (void)frame;
    (void)frame_cap;
    (void)quality;
    (void)rsl_dbm;
    assert(timeout_ms > 0u);
    *frame_len = 0u;
    rx_calls++;
    if (rx_calls == 1u) {
        *failure = injected_failure;
        now_ms += finish_on_error ? timeout_ms : 2u;
        return injected_ret;
    }
    assert(rx_calls == 2u);
    *failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    now_ms += timeout_ms;
    return -ETIMEDOUT;
}
int dwm3000_driver_send_frame_tracked_until(
    const uint8_t *frame, size_t frame_len, uint32_t timeout_ms,
    uint64_t absolute_deadline_ms, struct dwm3000_tx_observation *observation)
{
    (void)frame;
    (void)frame_len;
    (void)timeout_ms;
    (void)absolute_deadline_ms;
    (void)observation;
    assert(false); /* No malformed/colliding packet may create an ACK. */
    return -EIO;
}
static int app_gateway_enumeration_response_handle_bundle(
    const struct uwb_enumeration_bundle_frame *bundle, uint64_t received_at_ms,
    struct uwb_enumeration_hop_ack_frame *ack)
{
    (void)bundle;
    (void)received_at_ms;
    (void)ack;
    assert(false);
    return -EINVAL;
}
static int mesh_errno_from_proto(int ret) { return ret; }
static int mesh_radio_idle_with_bounded_recovery(const char *reason)
{
    (void)reason;
    parked++;
    return parking_ret;
}
static int mesh_rx_radio_finish(struct radio_guard_uwb_lease *lease, int ret)
{
    assert(lease->held);
    lease->held = false;
    released++;
    return ret < 0 ? ret : release_ret;
}
static void app_gateway_enumeration_response_publish_pending(void)
{
    assert(released == 1u);
    published++;
}
static void app_watchdog_note_radio_progress(void)
{
    assert(parked == 1u && released == 1u && parking_ret == 0);
    watchdog_feeds++;
}
static int app_node_comm_gateway_delivery_safe_boundary(void)
{
    safe_boundaries++;
    return 0;
}
static void mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    assert(delay_ms == 0u);
    scheduled++;
}

#include "gateway_compact_rx_production.inc"

static void reset_fixture(int ret, enum dwm3000_rx_failure failure)
{
    now_ms = 1000u;
    deadline_ms = now_ms + ENUMERATION_RESPONSE_ROUND_MS;
    rx_calls = configure_calls = recovery_calls = parked = released = 0u;
    watchdog_feeds = safe_boundaries = scheduled = published = 0u;
    injected_ret = ret;
    injected_failure = failure;
    recovery_ret = parking_ret = release_ret = 0;
    finish_on_error = false;
    mesh_uwb_rx_active = true;
}

static void test_collision_rearms_without_reset(void)
{
    const enum dwm3000_rx_failure failures[] = {
        DWM3000_RX_FAILURE_SFD_TIMEOUT, DWM3000_RX_FAILURE_FRAME_TIMEOUT,
        DWM3000_RX_FAILURE_CRC_OR_PHY, DWM3000_RX_FAILURE_BAD_FRAME,
    };
    for (size_t i = 0u; i < sizeof(failures) / sizeof(failures[0]); i++) {
        reset_fixture(-EIO, failures[i]);
        assert(mesh_gateway_run_enumeration_response_slice());
        assert(rx_calls == 2u && configure_calls == 1u && recovery_calls == 0u);
        assert(parked == 1u && released == 1u && published == 1u);
        assert(watchdog_feeds == 1u && scheduled == 1u);
        assert(now_ms == deadline_ms);

        /* A collision alone is not functional watchdog progress. */
        reset_fixture(-EIO, failures[i]);
        finish_on_error = true;
        assert(mesh_gateway_run_enumeration_response_slice());
        assert(rx_calls == 1u && recovery_calls == 0u && watchdog_feeds == 0u);
    }
    reset_fixture(-EMSGSIZE, DWM3000_RX_FAILURE_BAD_FRAME);
    assert(mesh_gateway_run_enumeration_response_slice());
    assert(recovery_calls == 0u && configure_calls == 1u);
}

static void test_hard_errors_keep_recovery_and_failed_cleanup_stops_progress(void)
{
    reset_fixture(-EIO, DWM3000_RX_FAILURE_NONE);
    assert(mesh_gateway_run_enumeration_response_slice());
    assert(recovery_calls == 1u && configure_calls == 2u && rx_calls == 2u);

    reset_fixture(-ENODEV, DWM3000_RX_FAILURE_NONE);
    recovery_ret = -EIO;
    assert(mesh_gateway_run_enumeration_response_slice());
    assert(recovery_calls == 1u && configure_calls == 1u && rx_calls == 1u);
    assert(parked == 1u && released == 1u && watchdog_feeds == 0u);

    reset_fixture(-EIO, DWM3000_RX_FAILURE_CRC_OR_PHY);
    parking_ret = -EIO;
    assert(mesh_gateway_run_enumeration_response_slice());
    assert(!mesh_uwb_rx_active && watchdog_feeds == 0u && scheduled == 0u);

    reset_fixture(-ECANCELED, DWM3000_RX_FAILURE_NONE);
    assert(mesh_gateway_run_enumeration_response_slice());
    assert(recovery_calls == 0u && rx_calls == 1u && safe_boundaries == 1u);
    assert(parked == 1u && released == 1u);
}

int main(void)
{
    test_collision_rearms_without_reset();
    test_hard_errors_keep_recovery_and_failed_cleanup_stops_progress();
    puts("gateway compact RX collision recovery: PASS");
    return 0;
}
