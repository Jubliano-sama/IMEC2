#include "app_stack_workload_diag.h"

#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)

#include "app_stack_diag.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define STACK_WORKLOAD_DIAG_MAX_RUNS APP_STACK_DIAG_MAX_ACTIVE_RUNS

_Static_assert(STACK_WORKLOAD_DIAG_MAX_RUNS >= APP_STACK_DIAG_COMBINED_PEAK_RUNS,
               "workload diagnostics must cover the combined peak");

struct stack_workload_diag_run {
    struct proto_packet packet;
    uint32_t run_id;
    enum app_stack_diag_workload workload;
    bool active;
};

static struct stack_workload_diag_run stack_workload_diag_runs[STACK_WORKLOAD_DIAG_MAX_RUNS];
K_MUTEX_DEFINE(stack_workload_diag_mutex);

#if defined(CONFIG_IMEC_STACK_WORKLOAD_DIAG_TEST_HOOKS)
void app_stack_workload_diag_test_lock_attempt(void);
#endif

static void stack_workload_diag_lock(void)
{
#if defined(CONFIG_IMEC_STACK_WORKLOAD_DIAG_TEST_HOOKS)
    app_stack_workload_diag_test_lock_attempt();
#endif
    k_mutex_lock(&stack_workload_diag_mutex, K_FOREVER);
}

static bool packet_matches(const struct proto_packet *left,
                           const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->msg_type == right->msg_type;
}

static struct app_stack_diag_state stack_workload_diag_state(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct app_stack_diag_state state = {0};

    if (pressure != NULL) {
        state.queue_depth = pressure->queue_depth;
        state.custody_depth = pressure->custody_depth;
        state.credit_available = pressure->credit_available;
        state.retry_depth = pressure->retry_depth;
        state.drain_depth = pressure->drain_depth;
    }

    if (packet != NULL) {
        state.source_id = packet->src_id;
        state.destination_id = packet->dst_id;
        state.session_id = packet->session_id;
        state.packet_sequence = packet->seq;
        state.message_type = packet->msg_type;
    }
    return state;
}

static struct stack_workload_diag_run *stack_workload_diag_find_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet)
{
    for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS; index++) {
        struct stack_workload_diag_run *run = &stack_workload_diag_runs[index];

        if (run->active && run->workload == workload &&
            packet_matches(&run->packet, packet)) {
            return run;
        }
    }
    return NULL;
}

static void stack_workload_diag_admit_locked(
    enum app_stack_diag_workload workload,
    enum app_stack_diag_owner owner,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *slot = NULL;
    struct app_stack_diag_state state;

    if (packet == NULL ||
        stack_workload_diag_find_locked(workload, packet) != NULL) {
        return;
    }
    for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS; index++) {
        if (!stack_workload_diag_runs[index].active) {
            slot = &stack_workload_diag_runs[index];
            break;
        }
    }
    if (slot == NULL) {
        state = stack_workload_diag_state(packet, pressure);
        (void)app_stack_diag_run_begin(workload, owner, &state);
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    slot->run_id = app_stack_diag_run_begin(workload, owner, &state);
    if (slot->run_id == 0u) {
        return;
    }
    slot->packet = *packet;
    slot->workload = workload;
    slot->active = true;
}

static void stack_workload_diag_sample_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *run =
        stack_workload_diag_find_locked(workload, packet);
    struct app_stack_diag_state state;

    if (run == NULL) {
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    (void)app_stack_diag_sample(run->run_id, &state);
}

static void stack_workload_diag_release_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *run =
        stack_workload_diag_find_locked(workload, packet);
    struct app_stack_diag_state state;

    if (run == NULL) {
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    if (app_stack_diag_run_end(run->run_id, outcome, &state) == 0) {
        memset(run, 0, sizeof(*run));
    }
}

static void stack_workload_diag_admit(
    enum app_stack_diag_workload workload,
    enum app_stack_diag_owner owner,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_lock();
    stack_workload_diag_admit_locked(workload, owner, packet, pressure);
    k_mutex_unlock(&stack_workload_diag_mutex);
}

static void stack_workload_diag_sample(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_lock();
    stack_workload_diag_sample_locked(workload, packet, pressure);
    k_mutex_unlock(&stack_workload_diag_mutex);
}

static void stack_workload_diag_release(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_lock();
    stack_workload_diag_release_locked(workload, packet, outcome, pressure);
    k_mutex_unlock(&stack_workload_diag_mutex);
}

#define DEFINE_WORKLOAD_DIAG(prefix, kind, owner) \
    void app_stack_workload_diag_##prefix##_admit(const struct proto_packet *packet, \
                                                  uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_admit(kind, owner, packet, &pressure); } \
    void app_stack_workload_diag_##prefix##_sample(const struct proto_packet *packet, \
                                                   uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_sample(kind, packet, &pressure); } \
    void app_stack_workload_diag_##prefix##_release(const struct proto_packet *packet, int result, \
                                                    uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_release(kind, packet, result == 0 ? APP_STACK_DIAG_TERMINAL_ACK : APP_STACK_DIAG_TERMINAL_ERROR, &pressure); }

DEFINE_WORKLOAD_DIAG(click, APP_STACK_DIAG_WORKLOAD_CLICK_SPAM,
                     APP_STACK_DIAG_OWNER_CLICKER_ACTION)
DEFINE_WORKLOAD_DIAG(cir, APP_STACK_DIAG_WORKLOAD_CIR_HANDLING,
                     APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN)
DEFINE_WORKLOAD_DIAG(relay, APP_STACK_DIAG_WORKLOAD_RELAY_RETRY,
                     APP_STACK_DIAG_OWNER_MESH_ROUTE)
DEFINE_WORKLOAD_DIAG(ble, APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE,
                     APP_STACK_DIAG_OWNER_BT_RX)
DEFINE_WORKLOAD_DIAG(click_activity, APP_STACK_DIAG_WORKLOAD_CLICK_ACTIVITY,
                     APP_STACK_DIAG_OWNER_CLICKER_ACTION)
DEFINE_WORKLOAD_DIAG(gateway_control,
                     APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL,
                     APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE)

void app_stack_workload_diag_anchor_scan_cycle(
    const struct proto_packet *packet,
    uint16_t queue_depth,
    uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };

    stack_workload_diag_lock();
    stack_workload_diag_admit_locked(
        APP_STACK_DIAG_WORKLOAD_ANCHOR_SCAN,
        APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN, packet, &pressure);
    stack_workload_diag_sample_locked(
        APP_STACK_DIAG_WORKLOAD_ANCHOR_SCAN, packet, &pressure);
    stack_workload_diag_release_locked(
        APP_STACK_DIAG_WORKLOAD_ANCHOR_SCAN, packet,
        APP_STACK_DIAG_TERMINAL_ACK, &pressure);
    k_mutex_unlock(&stack_workload_diag_mutex);
}

void app_stack_workload_diag_gateway_report_cycle(
    const struct proto_packet *packet,
    uint16_t queue_depth,
    uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };

    stack_workload_diag_lock();
    stack_workload_diag_admit_locked(
        APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS,
        APP_STACK_DIAG_OWNER_MESH_ROUTE, packet, &pressure);
    stack_workload_diag_sample_locked(
        APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS, packet, &pressure);
    stack_workload_diag_release_locked(
        APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS, packet,
        APP_STACK_DIAG_TERMINAL_ACK, &pressure);
    k_mutex_unlock(&stack_workload_diag_mutex);
}

void app_stack_workload_diag_ble_release_all(int result,
                                             uint16_t queue_depth,
                                             uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };

    app_stack_workload_diag_ble_release_all_with_pressure(
        result == 0 ? APP_STACK_DIAG_TERMINAL_ACK : APP_STACK_DIAG_TERMINAL_DISCONNECT,
        &pressure);
}

void app_stack_workload_diag_ble_release_all_with_pressure(
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_lock();
    for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS; index++) {
        struct stack_workload_diag_run *run = &stack_workload_diag_runs[index];

        if (run->active && run->workload == APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE) {
            stack_workload_diag_release_locked(run->workload, &run->packet,
                                               outcome, pressure);
        }
    }
    k_mutex_unlock(&stack_workload_diag_mutex);
}

void app_stack_workload_diag_ble_admit_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_owner owner,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_admit(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, owner,
                              packet, pressure);
}

void app_stack_workload_diag_ble_sample_with_pressure(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_sample(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, packet,
                               pressure);
}

void app_stack_workload_diag_ble_terminal_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_release(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, packet,
                                outcome, pressure);
}

#endif
