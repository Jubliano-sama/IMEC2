#include "dwm3000_driver.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DWM3000_RX_ERROR_STATUS_MASK \
    (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_RXOVRR_BIT_MASK)

struct completion {
    uint32_t elapsed_us;
    int frame_result;
};

static struct completion completions[8];
static size_t completion_count, completion_index;
static int64_t now_us;
static uint64_t physical_deadlines[8];
static uint32_t physical_budgets[8];
static uint64_t expected_absolute_deadline_ms;
static unsigned int waits, reads, timestamps, diagnostics, clears, arms, stops;
static bool rx_armed, radio_state_unknown, last_rx_host_uptime_valid;
static uint32_t last_rx_host_uptime_ms;
static struct dwm3000_driver_stats driver_stats;
static const uint8_t accepted_payload[] = { 0x12, 0x34, 0x56, 0x78 };
static uint32_t poll_status, poll_read_us, poll_entry_pause_us;
static unsigned int status_reads, poll_ready_after_read;
static int poll_port_error;
static bool use_production_status_poller;

static int wait_status_internal_production(uint32_t mask, uint32_t timeout_ms,
    uint32_t *status, bool log_events, uint64_t *observed_at_ms,
    uint64_t absolute_deadline_ms);

static int64_t k_uptime_get(void) { return now_us / 1000; }

void dwt_forcetrxoff(void)
{
    stops++;
    rx_armed = false;
    now_us += 50;
}

/* The physical waiter may observe a ready frame only after the workqueue
 * resumes. Its successful return does not establish timely consumption. */
static int wait_status_internal(uint32_t mask, uint32_t timeout_ms,
                                uint32_t *status, bool log_events,
                                uint64_t *observed_at_ms,
                                uint64_t absolute_deadline_ms)
{
    assert(mask == (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO |
                    DWM3000_RX_ERROR_STATUS_MASK));
    assert(log_events && rx_armed && timeout_ms > 0u);
    assert(absolute_deadline_ms == expected_absolute_deadline_ms);
    assert(waits < sizeof(physical_deadlines) / sizeof(physical_deadlines[0]));
    physical_budgets[waits] = timeout_ms;
    physical_deadlines[waits++] = (uint64_t)k_uptime_get() + timeout_ms;
    if (use_production_status_poller) {
        return wait_status_internal_production(mask, timeout_ms, status,
            log_events, observed_at_ms, absolute_deadline_ms);
    }
    if (completion_index == completion_count) {
        now_us += (int64_t)timeout_ms * 1000;
        *status = SYS_STATUS_RXFTO_BIT_MASK;
        return -ETIMEDOUT;
    }
    now_us += completions[completion_index++].elapsed_us;
    *observed_at_ms = (uint64_t)k_uptime_get();
    *status = SYS_STATUS_RXFCG_BIT_MASK;
    rx_armed = false;
    return 0;
}

static int read_rx_frame(uint8_t *buffer, size_t buffer_len, size_t *frame_len)
{
    assert(completion_index > 0u && !rx_armed);
    reads++;
    now_us += 200;
    if (completions[completion_index - 1u].frame_result < 0) {
        return completions[completion_index - 1u].frame_result;
    }
    assert(buffer_len >= sizeof(accepted_payload));
    memcpy(buffer, accepted_payload, sizeof(accepted_payload));
    *frame_len = sizeof(accepted_payload);
    return 0;
}

static uint64_t read_rx_timestamp_u64(void)
{
    timestamps++;
    now_us += 100;
    return UINT64_C(0x123456789a);
}

static int take_port_error(const char *operation)
{
    if (strcmp(operation, "rx-timestamp") == 0) {
        return 0;
    }
    assert(strcmp(operation, "status-read") == 0 ||
           strcmp(operation, "status-timeout-read") == 0);
    return poll_port_error;
}

static bool read_rx_diagnostics(int8_t *rsl_dbm,
                                uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                                bool *cir_sampled,
                                int16_t *clock_offset_raw,
                                bool *clock_offset_sampled,
                                int32_t *carrier_integrator,
                                bool *carrier_integrator_sampled,
                                uint64_t *ipatov_rx_timestamp)
{
    diagnostics++;
    now_us += 200;
    *rsl_dbm = -61;
    memset(cir_sample, 0x6a, UWB_CIR_SAMPLE_LEN);
    *cir_sampled = true;
    *clock_offset_raw = 123;
    *clock_offset_sampled = true;
    *carrier_integrator = 456;
    *carrier_integrator_sampled = true;
    *ipatov_rx_timestamp = UINT64_C(0x23456789ab);
    return true;
}

static int clear_status_checked(uint32_t mask, const char *operation)
{
    assert(mask == SYS_STATUS_RXFCG_BIT_MASK ||
           mask == (SYS_STATUS_ALL_RX_TO | DWM3000_RX_ERROR_STATUS_MASK));
    assert(operation != NULL);
    clears++;
    now_us += 100;
    return 0;
}

static int start_immediate_rx(void)
{
    assert(!rx_armed);
    arms++;
    now_us += 700;
    rx_armed = true;
    return 0;
}

/* A second seam executes the actual status poller. Hardware register reads
 * advance time, including an arbitrarily long scheduler/SPI pause. */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define IS_ENABLED(option) (option)
#define CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE 0
#define DWM3000_STATUS_POLL_INTERVAL_US 50u
typedef int atomic_val_t;
static int receive_abort_enabled, receive_abort_owners;
static int atomic_get(const int *value) { return *value; }
static int atomic_and(int *value, int mask)
{
    int previous = *value;
    *value &= mask;
    return previous;
}
static int64_t k_uptime_ticks(void) { return now_us; }
static int64_t k_ms_to_ticks_ceil64(uint32_t ms) { return (int64_t)ms * 1000; }
static uint32_t k_cycle_get_32(void) { return (uint32_t)now_us; }
static uint64_t k_cyc_to_us_floor64(uint32_t cycles) { return cycles; }
static void k_busy_wait(uint32_t us) { now_us += us; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static int check_device_fatal_status(const char *operation)
{
    assert(operation != NULL);
    return 0;
}
static void dwm3000_port_clear_error(void) { now_us += poll_entry_pause_us; }
uint32_t dwt_read32bitoffsetreg(int address, int offset)
{
    assert(address == SYS_STATUS_ID && offset == 0);
    status_reads++;
    now_us += poll_read_us;
    return status_reads >= poll_ready_after_read ? poll_status : 0u;
}
#define wait_status_internal wait_status_internal_production
#include "dwm3000_range_absolute_status_production.inc"
#undef wait_status_internal

/* Actual receive_frame, its scope-drop rearm, and remaining_timeout_ms are
 * extracted from production. Only physical operations and elapsed time above
 * are modeled; in particular this fixture does not clamp any RX budget. */
#include "dwm3000_range_absolute_rx_production.inc"

struct received {
    uint8_t bytes[16];
    size_t length;
    uint32_t status;
    uint64_t timestamp;
    uint8_t quality;
    int8_t rsl;
    uint8_t cir[UWB_CIR_SAMPLE_LEN];
    bool cir_sampled;
    int16_t clock_offset;
    bool clock_sampled;
    int32_t carrier;
    bool carrier_sampled;
    uint64_t ipatov;
};

static void reset(uint64_t start_ms)
{
    assert(start_ms < (uint64_t)INT64_MAX / 1000u);
    now_us = (int64_t)start_ms * 1000;
    completion_count = completion_index = 0u;
    waits = reads = timestamps = diagnostics = clears = arms = stops = 0u;
    memset(&driver_stats, 0, sizeof(driver_stats));
    memset(physical_deadlines, 0, sizeof(physical_deadlines));
    memset(physical_budgets, 0, sizeof(physical_budgets));
    poll_status = poll_read_us = poll_entry_pause_us = status_reads = 0u;
    poll_ready_after_read = 0u;
    poll_port_error = receive_abort_enabled = receive_abort_owners = 0;
    use_production_status_poller = false;
    rx_armed = true;
    radio_state_unknown = false;
    last_rx_host_uptime_valid = false;
    last_rx_host_uptime_ms = 0u;
}

static void append_completion(uint32_t elapsed_us, int frame_result)
{
    assert(completion_count < sizeof(completions) / sizeof(completions[0]));
    completions[completion_count++] = (struct completion) {
        .elapsed_us = elapsed_us, .frame_result = frame_result,
    };
}

static int receive(uint32_t timeout_ms, uint64_t absolute_deadline_ms,
                   struct received *result)
{
    memset(result->bytes, 0xaa, sizeof(result->bytes));
    memset(result->cir, 0xaa, sizeof(result->cir));
    result->length = 99u;
    result->status = UINT32_MAX;
    result->timestamp = UINT64_MAX;
    result->quality = 17u;
    result->rsl = -17;
    result->cir_sampled = false;
    result->clock_offset = -17;
    result->clock_sampled = false;
    result->carrier = -17;
    result->carrier_sampled = false;
    result->ipatov = UINT64_MAX;
    expected_absolute_deadline_ms = absolute_deadline_ms;
    if (absolute_deadline_ms != 0u &&
        (uint64_t)k_uptime_get() + timeout_ms < absolute_deadline_ms) {
        expected_absolute_deadline_ms = (uint64_t)k_uptime_get() + timeout_ms;
    }
    return receive_frame(timeout_ms, &result->status,
        result->bytes, sizeof(result->bytes), &result->length,
        &result->timestamp, &result->quality, &result->rsl, result->cir,
        &result->cir_sampled, &result->clock_offset, &result->clock_sampled,
        &result->carrier, &result->carrier_sampled, &result->ipatov, true,
        absolute_deadline_ms);
}

static void assert_no_acceptance(const struct received *result)
{
    assert(result->length == 0u);
    assert(!last_rx_host_uptime_valid && last_rx_host_uptime_ms == 0u);
    assert(timestamps == 0u && diagnostics == 0u);
    assert(result->timestamp == UINT64_MAX && result->ipatov == UINT64_MAX);
    assert(result->quality == 17u && result->rsl == -17);
    assert(result->clock_offset == -17 && result->carrier == -17);
    assert(!result->cir_sampled && !result->clock_sampled &&
           !result->carrier_sampled);
    for (size_t i = 0u; i < sizeof(result->bytes); i++) {
        assert(result->bytes[i] == 0xaa);
    }
    for (size_t i = 0u; i < sizeof(result->cir); i++) {
        assert(result->cir[i] == 0xaa);
    }
    assert(driver_stats.rx_starts == 1u && driver_stats.rx_timeouts == 1u);
    assert(driver_stats.rx_dones == 0u && driver_stats.rx_failures == 0u);
    assert(stops == 1u && !rx_armed);
}

static void test_delayed_entry_and_completion(uint64_t base_ms)
{
    struct received result;

    /* A budget computed before the scheduler pause must not reopen the slot. */
    reset(base_ms + 5u);
    append_completion(0u, 0);
    assert(receive(55u, base_ms + 5u, &result) == -ETIMEDOUT);
    assert(waits == 0u && reads == 0u && result.status == 0u);
    assert_no_acceptance(&result);

    reset(base_ms + 4u);
    assert(receive(55u, base_ms + 5u, &result) == -ETIMEDOUT);
    assert(waits == 1u && physical_budgets[0] == 1u);
    assert(physical_deadlines[0] == base_ms + 5u && reads == 0u);
    assert((uint64_t)k_uptime_get() == base_ms + 5u);
    assert_no_acceptance(&result);

    /* A ready status returned at or after expiry must not expose RX bytes or
     * metadata, even though the lower waiter reports successful RF activity. */
    for (uint32_t delay_ms = 1u; delay_ms <= 2u; delay_ms++) {
        reset(base_ms + 4u);
        append_completion(delay_ms * 1000u, 0);
        assert(receive(55u, base_ms + 5u, &result) == -ETIMEDOUT);
        assert(waits == 1u && physical_budgets[0] == 1u && reads == 0u);
        assert(physical_deadlines[0] == base_ms + 5u && result.status == 0u);
        assert_no_acceptance(&result);
    }
}

static void test_scope_noise_preserves_end(uint64_t base_ms,
                                          uint64_t absolute_deadline_ms)
{
    struct received result;
    const uint32_t timeout_ms = absolute_deadline_ms == 0u ? 10u : 55u;

    reset(base_ms);
    /* Each foreign packet consumes 2 ms waiting and 1 ms of read/clear/rearm.
     * A matching packet at the original end remains ineligible. */
    for (unsigned int i = 0u; i < 3u; i++) {
        append_completion(2000u, -EHOSTUNREACH);
    }
    if (absolute_deadline_ms != 0u) {
        append_completion(1000u, 0);
    }
    assert(receive(timeout_ms, absolute_deadline_ms, &result) == -ETIMEDOUT);
    assert(waits == 4u && reads == 3u && arms == 3u && clears == 3u);
    for (unsigned int i = 0u; i < waits; i++) {
        assert(physical_deadlines[i] == base_ms + 10u);
        assert(physical_budgets[i] == 10u - 3u * i);
    }
    assert((uint64_t)k_uptime_get() == base_ms + 10u);
    assert_no_acceptance(&result);
}

static void test_accepted_frame_and_shorter_relative_budget(void)
{
    struct received result;
    reset(100u);
    append_completion(2000u, 0);
    assert(receive(55u, 105u, &result) == 0);
    assert(waits == 1u && physical_budgets[0] == 5u);
    assert(reads == 1u && timestamps == 1u && diagnostics == 1u);
    assert(result.length == sizeof(accepted_payload));
    assert(memcmp(result.bytes, accepted_payload, result.length) == 0);
    assert(last_rx_host_uptime_valid && last_rx_host_uptime_ms == 102u);
    assert(result.timestamp == UINT64_C(0x123456789a));
    assert(result.quality == 100u && result.rsl == -61);
    assert(result.cir_sampled && result.clock_sampled && result.carrier_sampled);
    assert(driver_stats.rx_dones == 1u && driver_stats.rx_timeouts == 0u);

    reset(100u);
    assert(receive(2u, 105u, &result) == -ETIMEDOUT);
    assert(waits == 1u && physical_budgets[0] == 2u);
    assert(physical_deadlines[0] == 102u);
    assert_no_acceptance(&result);

    /* The phase expires before the overall request. Resume after that phase
     * end inside the real status poller: its original 102 ms endpoint must
     * survive the pause, even though the request remains valid until 105. */
    reset(100u);
    use_production_status_poller = true;
    poll_entry_pause_us = 3000u;
    poll_status = SYS_STATUS_RXFCG_BIT_MASK;
    assert(receive(2u, 105u, &result) == -ETIMEDOUT);
    assert(waits == 1u && physical_budgets[0] == 2u);
    assert(physical_deadlines[0] == 102u);
    assert(status_reads == 0u && reads == 0u && result.status == 0u);
    assert((uint64_t)k_uptime_get() == 103u);
    assert_no_acceptance(&result);

    reset(100u);
    append_completion(2000u, 0);
    assert(receive(55u, 0u, &result) == 0);
    assert(waits == 1u && physical_budgets[0] == 55u);
    assert(physical_deadlines[0] == 155u && driver_stats.rx_dones == 1u);
}

static void test_actual_status_poller(uint64_t base_ms)
{
    const uint32_t ready[] = { SYS_STATUS_RXFCG_BIT_MASK,
                               SYS_STATUS_TXFRS_BIT_MASK };
    uint32_t status;
    uint64_t observed_at_ms;

    for (size_t i = 0u; i < sizeof(ready) / sizeof(ready[0]); i++) {
        /* A scheduler pause before the first register poll cannot resurrect
         * either RX completion or the subsequent range TX completion wait. */
        reset(base_ms);
        poll_status = ready[i];
        poll_entry_pause_us = 6000u;
        status = UINT32_MAX;
        observed_at_ms = UINT64_MAX;
        assert(wait_status_internal_production(ready[i], 55u, &status, true,
            &observed_at_ms, base_ms + 5u) == -ETIMEDOUT);
        assert(status_reads == 0u && status == 0u && observed_at_ms == 0u);

        for (uint32_t elapsed_ms = 5u; elapsed_ms <= 6u; elapsed_ms++) {
            reset(base_ms);
            poll_status = ready[i];
            poll_read_us = elapsed_ms * 1000u;
            status = UINT32_MAX;
            observed_at_ms = UINT64_MAX;
            assert(wait_status_internal_production(ready[i], 55u, &status, true,
                &observed_at_ms, base_ms + 5u) == -ETIMEDOUT);
            assert(status_reads == 1u && status == 0u && observed_at_ms == 0u);
            assert(driver_stats.sys_status_poll_timeouts == 1u);
        }

        reset(base_ms);
        poll_status = ready[i];
        poll_read_us = 1000u;
        assert(wait_status_internal_production(ready[i], 55u, &status, true,
            &observed_at_ms, base_ms + 5u) == 0);
        assert(status_reads == 1u && status == ready[i]);
        assert(observed_at_ms == base_ms + 1u);

        /* A hardware error still wins when the failed SPI read took the
         * remaining budget; it must not become ordinary RF silence. */
        reset(base_ms);
        poll_status = ready[i];
        poll_read_us = 6000u;
        poll_port_error = -EIO;
        assert(wait_status_internal_production(ready[i], 55u, &status, true,
            &observed_at_ms, base_ms + 5u) == -EIO);
        assert(status_reads == 1u && status == 0u && observed_at_ms == 0u);
        assert(driver_stats.sys_status_poll_timeouts == 0u);

        /* The relative timeout's final status sample is another physical
         * read, and it must obey the same absolute boundary. */
        reset(base_ms);
        poll_status = ready[i];
        poll_ready_after_read = 2u;
        poll_read_us = 3000u;
        assert(wait_status_internal_production(ready[i], 1u, &status, true,
            &observed_at_ms, base_ms + 5u) == -ETIMEDOUT);
        assert(status_reads == 2u && status == 0u && observed_at_ms == 0u);
        assert(driver_stats.sys_status_poll_timeouts == 1u);
    }

    reset(base_ms);
    poll_status = 0u;
    poll_read_us = 100u;
    assert(wait_status_internal_production(SYS_STATUS_RXFCG_BIT_MASK, 55u,
        &status, true, &observed_at_ms, base_ms + 5u) == -ETIMEDOUT);
    assert((uint64_t)k_uptime_get() == base_ms + 5u);
    assert(status_reads > 1u && status_reads < 100u && observed_at_ms == 0u);

    reset(base_ms);
    poll_status = SYS_STATUS_RXFCG_BIT_MASK;
    poll_read_us = 6000u;
    assert(wait_status_internal_production(SYS_STATUS_RXFCG_BIT_MASK, 55u,
        &status, true, &observed_at_ms, 0u) == 0);
    assert(status == SYS_STATUS_RXFCG_BIT_MASK && observed_at_ms == base_ms + 6u);
}

int main(void)
{
    const uint64_t bases[] = { 100u, UINT32_MAX - UINT64_C(3),
                              UINT32_MAX + UINT64_C(100) };
    for (size_t i = 0u; i < sizeof(bases) / sizeof(bases[0]); i++) {
        test_delayed_entry_and_completion(bases[i]);
        test_scope_noise_preserves_end(bases[i], bases[i] + 10u);
        test_scope_noise_preserves_end(bases[i], 0u);
        test_actual_status_poller(bases[i]);
    }
    test_accepted_frame_and_shorter_relative_budget();
    puts("production range RX absolute deadlines passed");
    return 0;
}
