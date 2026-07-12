#ifndef MESH_RUNTIME_H
#define MESH_RUNTIME_H

#include "mesh_preemption.h"
#include "mesh_relay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_RUNTIME_WORK_CAPACITY 16u

enum mesh_runtime_status {
    MESH_RUNTIME_OK = 0,
    MESH_RUNTIME_ERR_ARG = -3000,
    MESH_RUNTIME_ERR_CAPACITY = -3001,
    MESH_RUNTIME_ERR_RADIO_BUSY = -3002,
    MESH_RUNTIME_ERR_STATE = -3003,
};

enum mesh_runtime_work_kind {
    MESH_RUNTIME_WORK_GATEWAY_COMMAND = 0,
    MESH_RUNTIME_WORK_LOCAL_CLICK,
    MESH_RUNTIME_WORK_EVENT_REPAIR,
    MESH_RUNTIME_WORK_TRANSIT,
};

enum mesh_runtime_radio_owner {
    MESH_RUNTIME_RADIO_NONE = 0,
    MESH_RUNTIME_RADIO_LOW_DUTY_SCAN,
    MESH_RUNTIME_RADIO_CHANNEL9_EVENT,
    MESH_RUNTIME_RADIO_DS_TWR,
    MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
    MESH_RUNTIME_RADIO_LOCAL_REPORT,
    MESH_RUNTIME_RADIO_TRANSIT,
};

enum mesh_runtime_action_kind {
    MESH_RUNTIME_ACTION_NONE = 0,
    MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
    MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
    MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT,
    MESH_RUNTIME_ACTION_RUN_TRANSIT,
    MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY,
};

struct mesh_runtime_work {
    uint64_t ready_us;
    uint64_t token;
    uint32_t enqueue_order;
    enum mesh_runtime_work_kind kind;
    bool valid;
};

struct mesh_runtime_action {
    uint64_t token;
    uint64_t peer_id;
    uint64_t runnable_at_us;
    enum mesh_runtime_action_kind kind;
    struct mesh_click_preempt_plan click_preemption;
    bool transit_reservation_abandoned;
};

struct mesh_runtime_ops {
    int (*schedule)(enum mesh_runtime_work_kind kind,
                    uint64_t token,
                    uint64_t at_us,
                    void *ctx);
    void (*trace)(enum mesh_runtime_action_kind action,
                  uint64_t token,
                  uint64_t at_us,
                  void *ctx);
    void *ctx;
};

struct mesh_runtime {
    struct mesh_relay *relay;
    struct mesh_runtime_ops ops;
    struct mesh_runtime_work work[MESH_RUNTIME_WORK_CAPACITY];
    struct mesh_outbound transit;
    uint64_t local_id;
    uint64_t radio_busy_until_us;
    uint64_t selected_repair_peer_id;
    uint64_t repair_requested_us;
    uint32_t repair_original_retry_after_ms;
    uint32_t next_enqueue_order;
    uint32_t transit_abandon_count;
    uint32_t route_discovery_count;
    enum mesh_runtime_radio_owner radio_owner;
    bool transit_reserved;
    bool event_repair_pending;
};

void mesh_runtime_init(struct mesh_runtime *runtime,
                       struct mesh_relay *relay,
                       uint64_t local_id,
                       const struct mesh_runtime_ops *ops);
int mesh_runtime_submit(struct mesh_runtime *runtime,
                        enum mesh_runtime_work_kind kind,
                        uint64_t token,
                        uint64_t ready_us);
int mesh_runtime_reserve_transit(struct mesh_runtime *runtime,
                                 const struct mesh_outbound *outbound,
                                 uint64_t ready_us);
int mesh_runtime_claim_radio(struct mesh_runtime *runtime,
                             enum mesh_runtime_radio_owner owner,
                             uint64_t start_us,
                             uint64_t end_us);
int mesh_runtime_release_radio(struct mesh_runtime *runtime,
                               enum mesh_runtime_radio_owner owner,
                               uint64_t now_us);
bool mesh_runtime_radio_safe(const struct mesh_runtime *runtime,
                             uint64_t now_us);
int mesh_runtime_run_boundary(struct mesh_runtime *runtime,
                              uint64_t now_us,
                              struct mesh_runtime_action *action);
int mesh_runtime_handle_ack_timeout(struct mesh_runtime *runtime,
                                    uint32_t now_ms,
                                    uint32_t random_value,
                                    struct mesh_relay_result *core_result);
int mesh_runtime_complete_event_repair(struct mesh_runtime *runtime,
                                       const struct mesh_event_timing *timing,
                                       uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
