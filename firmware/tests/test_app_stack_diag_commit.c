#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_BOARD_H
int status_stack_diag_transaction_begin(void);
int status_stack_diag_note(const char *text);
void status_stack_diag_transaction_end(void);

#define APP_CONFIG_H
#define CONFIG_IMEC_STACK_DIAGNOSTICS 1
#define CONFIG_THREAD_NAME 1
#define CONFIG_ISR_STACK_SIZE 320
#define IMEC_BUILD_PRESET_NAME "mesh_gateway"
#define IMEC_STACK_DIAG_BUILD_ID "test-build"

#include "../app/src/app_stack_diag.c"

#define CAPTURE_RECORDS 16u

static struct k_thread fake_threads[] = {
    { .stack_info = { .size = 4096u }, .name = "boot", .unused = 1024u },
    { .stack_info = { .size = 1024u }, .name = "logging", .unused = 300u },
    { .stack_info = { .size = 1088u }, .name = "BT RX WQ", .unused = 260u },
};
static char captured[CAPTURE_RECORDS][APP_STACK_DIAG_RECORD_CAPACITY];
static size_t captured_count;
static unsigned write_position;
static unsigned fail_write_position;
static bool transaction_active;
static uint32_t fake_uptime;
static uint32_t fake_random = 1u;

k_tid_t zephyr_shim_current_thread(void)
{
    return &fake_threads[0];
}

void zephyr_shim_thread_foreach(
    void (*callback)(const struct k_thread *, void *),
    void *user_data)
{
    for (size_t index = 0u; index < ARRAY_SIZE(fake_threads); index++) {
        callback(&fake_threads[index], user_data);
    }
}

int64_t k_uptime_get(void)
{
    return fake_uptime++;
}

uint32_t sys_rand32_get(void)
{
    return fake_random++;
}

int status_stack_diag_transaction_begin(void)
{
    assert(!transaction_active);
    transaction_active = true;
    captured_count = 0u;
    write_position = 0u;
    return 0;
}

int status_stack_diag_note(const char *text)
{
    assert(transaction_active);
    assert(text != NULL);
    write_position++;
    if (write_position == fail_write_position) {
        return -EIO;
    }
    assert(captured_count < CAPTURE_RECORDS);
    assert(strlen(text) < sizeof(captured[captured_count]));
    strcpy(captured[captured_count++], text);
    return 0;
}

void status_stack_diag_transaction_end(void)
{
    assert(transaction_active);
    transaction_active = false;
}

static bool capture_contains(const char *needle)
{
    for (size_t index = 0u; index < captured_count; index++) {
        if (strstr(captured[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static struct app_stack_diag_state state(uint32_t identity)
{
    return (struct app_stack_diag_state){
        .queue_depth = 2u,
        .custody_depth = 1u,
        .credit_available = 1u,
        .source_id = 100u + identity,
        .destination_id = 200u,
        .session_id = 300u + identity,
        .packet_sequence = (uint16_t)identity,
        .message_type = 0x20u,
    };
}

static void test_run_begin_failure_rolls_back_identity(void)
{
    struct app_stack_diag_state current = state(1u);
    uint32_t run_id;

    fail_write_position = 1u;
    run_id = app_stack_diag_run_begin(APP_STACK_DIAG_WORKLOAD_CLICK_SPAM,
                                      APP_STACK_DIAG_OWNER_CLICKER_ACTION,
                                      &current);
    assert(run_id == 0u);
    assert(!transaction_active);

    fail_write_position = 0u;
    run_id = app_stack_diag_run_begin(APP_STACK_DIAG_WORKLOAD_CLICK_SPAM,
                                      APP_STACK_DIAG_OWNER_CLICKER_ACTION,
                                      &current);
    assert(run_id == 1u);
    assert(capture_contains("run=1"));
    assert(capture_contains("sequence=1 previous=0"));
    assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                  &current) == 0);
}

static void test_sample_failure_never_commits_end_or_count(void)
{
    for (unsigned failed_position = 1u; failed_position <= 6u;
         failed_position++) {
        struct app_stack_diag_state current = state(10u + failed_position);
        uint32_t run_id = app_stack_diag_run_begin(
            APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS,
            APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &current);

        assert(run_id != 0u);
        fail_write_position = failed_position;
        assert(app_stack_diag_sample(run_id, &current) == -EIO);
        assert(!capture_contains("DBG_STACK_SAMPLE_END"));

        fail_write_position = 0u;
        assert(app_stack_diag_sample(run_id, &current) == 0);
        assert(capture_contains("DBG_STACK_SAMPLE_END"));
        assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                      &current) == 0);
        assert(capture_contains("samples=1"));
    }
}

static void test_run_end_failure_retains_run_for_retry(void)
{
    struct app_stack_diag_state current = state(30u);
    uint32_t run_id = app_stack_diag_run_begin(
        APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL,
        APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &current);

    assert(run_id != 0u);
    assert(app_stack_diag_sample(run_id, &current) == 0);
    fail_write_position = 1u;
    assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                  &current) == -EIO);
    assert(!capture_contains("DBG_STACK_RUN_END"));

    fail_write_position = 0u;
    assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                  &current) == 0);
    assert(capture_contains("DBG_STACK_RUN_END"));
    assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                  &current) == -ENOENT);
}

static void test_thousand_run_commit_pressure(void)
{
    for (uint32_t iteration = 0u; iteration < 1000u; iteration++) {
        struct app_stack_diag_state current = state(100u + iteration);
        uint32_t run_id = app_stack_diag_run_begin(
            APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS,
            APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &current);

        assert(run_id != 0u);
        assert(app_stack_diag_sample(run_id, &current) == 0);
        assert(app_stack_diag_run_end(run_id, APP_STACK_DIAG_TERMINAL_ACK,
                                      &current) == 0);
    }
}

int main(void)
{
    app_stack_diag_start();
    assert(capture_contains("DBG_STACK_BOOT"));
    test_run_begin_failure_rolls_back_identity();
    test_sample_failure_never_commits_end_or_count();
    test_run_end_failure_retains_run_for_retry();
    test_thousand_run_commit_pressure();
    return 0;
}
