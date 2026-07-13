#include "app_stack_diag.h"
#include "app_stack_workload_diag.h"

#include <assert.h>
#include <stdint.h>

#define MAX_RECORDS 16u

static uint32_t next_run_id;
static uint32_t begin_count;
static uint32_t sample_count;
static uint32_t end_count;
static enum app_stack_diag_owner owners[MAX_RECORDS];
static enum app_stack_diag_terminal_outcome outcomes[MAX_RECORDS];
static struct app_stack_diag_state begin_states[MAX_RECORDS];
static struct app_stack_diag_state sample_states[MAX_RECORDS];
static struct app_stack_diag_state end_states[MAX_RECORDS];

void app_stack_diag_start(void)
{
}

uint32_t app_stack_diag_run_begin(enum app_stack_diag_workload workload,
                                  enum app_stack_diag_owner owner,
                                  const struct app_stack_diag_state *state)
{
    assert(workload <= APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE);
    assert(owner != APP_STACK_DIAG_OWNER_SHARED_MIN);
    assert(begin_count < MAX_RECORDS);
    owners[begin_count] = owner;
    begin_states[begin_count++] = *state;
    /* The sixth admission models the core capacity refusal without silent loss. */
    return begin_count > APP_STACK_DIAG_COMBINED_PEAK_RUNS ? 0u : ++next_run_id;
}

void app_stack_diag_sample(uint32_t run_id, const struct app_stack_diag_state *state)
{
    assert(run_id != 0u);
    assert(sample_count < MAX_RECORDS);
    sample_states[sample_count++] = *state;
}

void app_stack_diag_run_end(uint32_t run_id,
                            enum app_stack_diag_terminal_outcome outcome,
                            const struct app_stack_diag_state *state)
{
    assert(run_id != 0u);
    assert(outcome <= APP_STACK_DIAG_TERMINAL_ERROR);
    assert(end_count < MAX_RECORDS);
    outcomes[end_count] = outcome;
    end_states[end_count++] = *state;
}

static struct proto_packet packet(uint64_t source, uint32_t session, uint16_t seq)
{
    return (struct proto_packet){
        .src_id = source,
        .dst_id = 7u,
        .session_id = session,
        .seq = seq,
        .msg_type = MSG_CLICK_REPORT,
    };
}

int main(void)
{
    struct proto_packet first = packet(1u, 10u, 1u);
    struct proto_packet second = packet(1u, 11u, 2u);
    struct proto_packet cir = packet(2u, 12u, 3u);
    struct proto_packet relay = packet(3u, 13u, 4u);
    struct proto_packet ble = packet(4u, 14u, 5u);
    struct proto_packet overflow = packet(5u, 15u, 6u);
    const struct app_stack_workload_diag_pressure ble_pressure = {
        .queue_depth = 7u,
        .custody_depth = 3u,
        .credit_available = 1u,
        .retry_depth = 2u,
        .drain_depth = 4u,
    };

    app_stack_workload_diag_click_admit(&first, 1u, 1u);
    app_stack_workload_diag_click_admit(&second, 2u, 2u);
    app_stack_workload_diag_cir_admit(&cir, 3u, 3u);
    app_stack_workload_diag_relay_admit(&relay, 4u, 4u);
    app_stack_workload_diag_ble_admit_with_pressure(
        &ble, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &ble_pressure);
    app_stack_workload_diag_click_admit(&first, 5u, 5u);
    assert(begin_count == APP_STACK_DIAG_COMBINED_PEAK_RUNS);
    assert(owners[4] == APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE);
    assert(begin_states[4].credit_available == 1u);
    assert(begin_states[4].retry_depth == 2u);
    assert(begin_states[4].drain_depth == 4u);

    app_stack_workload_diag_ble_sample_with_pressure(&ble, &ble_pressure);
    app_stack_workload_diag_click_sample(&second, 2u, 2u);
    assert(sample_count == 2u);

    app_stack_workload_diag_click_release(&first, 0, 4u, 4u);
    app_stack_workload_diag_cir_release(&cir, -1, 3u, 3u);
    app_stack_workload_diag_relay_release(&relay, 0, 2u, 2u);
    app_stack_workload_diag_ble_terminal_with_pressure(
        &ble, APP_STACK_DIAG_TERMINAL_DISCONNECT, &ble_pressure);
    assert(end_count == 4u);
    assert(outcomes[0] == APP_STACK_DIAG_TERMINAL_ACK);
    assert(outcomes[1] == APP_STACK_DIAG_TERMINAL_ERROR);
    assert(outcomes[3] == APP_STACK_DIAG_TERMINAL_DISCONNECT);

    app_stack_workload_diag_click_release(&second, 0, 0u, 0u);
    app_stack_workload_diag_click_admit(&overflow, 1u, 1u);
    assert(end_count == 5u);
    assert(begin_count == 6u);
    app_stack_workload_diag_click_release(&overflow, 0, 0u, 0u);
    assert(end_count == 5u);
    return 0;
}
