#ifndef APP_STACK_DIAG_H
#define APP_STACK_DIAG_H

#include <stdint.h>

/* Two clicks plus CIR, relay retry, and BLE backpressure must overlap safely. */
#define APP_STACK_DIAG_COMBINED_PEAK_RUNS 5u
#define APP_STACK_DIAG_MAX_ACTIVE_RUNS APP_STACK_DIAG_COMBINED_PEAK_RUNS

enum app_stack_diag_workload {
    APP_STACK_DIAG_WORKLOAD_CLICK_SPAM = 0,
    APP_STACK_DIAG_WORKLOAD_CIR_HANDLING,
    APP_STACK_DIAG_WORKLOAD_RELAY_RETRY,
    APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE,
    APP_STACK_DIAG_WORKLOAD_CLICK_ACTIVITY,
    APP_STACK_DIAG_WORKLOAD_ANCHOR_SCAN,
    APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS,
    APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL,
};

enum app_stack_diag_owner {
    APP_STACK_DIAG_OWNER_CLICKER_ACTION = 0,
    APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN,
    APP_STACK_DIAG_OWNER_MESH_ROUTE,
    APP_STACK_DIAG_OWNER_BT_RX,
    APP_STACK_DIAG_OWNER_SHARED_MIN,
    APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE,
};

enum app_stack_diag_terminal_outcome {
    APP_STACK_DIAG_TERMINAL_ACK = 0,
    APP_STACK_DIAG_TERMINAL_CUSTODY_DROP,
    APP_STACK_DIAG_TERMINAL_DIRECT_ACK_FAILURE,
    APP_STACK_DIAG_TERMINAL_PREEMPTED,
    APP_STACK_DIAG_TERMINAL_TIMEOUT_DROP,
    APP_STACK_DIAG_TERMINAL_DISCONNECT,
    APP_STACK_DIAG_TERMINAL_ERROR,
};

struct app_stack_diag_state {
    uint16_t queue_depth;
    uint16_t custody_depth;
    uint16_t credit_available;
    uint16_t retry_depth;
    uint16_t drain_depth;
    uint64_t source_id;
    uint64_t destination_id;
    uint32_t session_id;
    uint16_t packet_sequence;
    uint8_t message_type;
};

void app_stack_diag_start(void);
uint32_t app_stack_diag_run_begin(enum app_stack_diag_workload workload,
                                  enum app_stack_diag_owner owner,
                                  const struct app_stack_diag_state *state);
int app_stack_diag_sample(uint32_t run_id,
                          const struct app_stack_diag_state *state);
int app_stack_diag_run_end(uint32_t run_id,
                           enum app_stack_diag_terminal_outcome outcome,
                           const struct app_stack_diag_state *state);

#endif
