#ifndef MESH_SIM_INTERNAL_H
#define MESH_SIM_INTERNAL_H

#include "mesh_sim.h"

enum mesh_sim_event_type {
    SIM_EVENT_TX_END = 0,
    SIM_EVENT_TX_EVALUATE = 1,
    SIM_EVENT_RX_END = 2,
    SIM_EVENT_CONNECTION_END = 3,
    SIM_EVENT_CONNECTION_REPAIR_END = 4,
    SIM_EVENT_RELAY_TICK = 5,
    SIM_EVENT_CONNECTION_START = 6,
    SIM_EVENT_CONNECTION_REPAIR_START = 7,
    SIM_EVENT_RX_START = 8,
    SIM_EVENT_TX_START = 9,
    SIM_EVENT_LOW_DUTY_START = 10,
    SIM_EVENT_RUNTIME_BOUNDARY = 11,
    SIM_EVENT_RUNTIME_RADIO_RELEASE = 12,
    SIM_EVENT_WATCHDOG_EXPIRE = 13,
    SIM_EVENT_ROUTE_DISCOVERY_RETRY = 14,
    SIM_EVENT_TRACE_MARKER = 15,
};

int mesh_sim_fail(struct mesh_sim_world *world, int status);
bool mesh_sim_node_index_valid(const struct mesh_sim_world *world,
                               uint8_t node_index);
uint32_t mesh_sim_time_ms(uint64_t time_us);
bool mesh_sim_has_peer_work(const struct mesh_sim_role_instance *node,
                            uint64_t peer_id);
bool mesh_sim_has_active_relay_to(const struct mesh_sim_role_instance *node,
                                  uint64_t peer_id);
int mesh_sim_publish_connection_timing(
    struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection);
void mesh_sim_clear_connection_timing(
    struct mesh_sim_world *world,
    const struct mesh_sim_connection *connection);
enum dwm3000_timing_phy mesh_sim_radio_timing_phy(enum mesh_sim_phy phy);
int mesh_sim_events_runtime_schedule_cb(enum mesh_runtime_work_kind kind,
                                        uint64_t token,
                                        uint64_t at_us,
                                        void *ctx);
void mesh_sim_events_runtime_trace_cb(enum mesh_runtime_action_kind action,
                                      uint64_t token,
                                      uint64_t at_us,
                                      void *ctx);

int mesh_sim_trace_add(struct mesh_sim_world *world,
                       uint64_t time_us,
                       uint64_t node_id,
                       uint64_t peer_id,
                       enum mesh_sim_transition_kind kind,
                       uint8_t msg_type,
                       uint32_t detail);
int mesh_sim_telemetry_reserve_connection_event(
    struct mesh_sim_world *world,
    uint16_t *event_index);
int mesh_sim_telemetry_reserve_rx_window(struct mesh_sim_world *world,
                                         uint16_t *window_index);
int mesh_sim_telemetry_reserve_transmission(
    struct mesh_sim_world *world,
    uint16_t *transmission_index);
int mesh_sim_telemetry_reserve_reception(struct mesh_sim_world *world,
                                         uint16_t *reception_index);

int mesh_sim_scheduler_schedule(struct mesh_sim_world *world,
                                enum mesh_sim_event_type type,
                                uint64_t at_us,
                                uint16_t object_index);
int mesh_sim_scheduler_schedule_priority(struct mesh_sim_world *world,
                                         enum mesh_sim_event_type type,
                                         uint64_t at_us,
                                         uint16_t object_index,
                                         uint8_t priority,
                                         uint32_t token);
int mesh_sim_scheduler_reschedule_watchdog(struct mesh_sim_world *world,
                                           uint8_t node_index,
                                           uint64_t deadline_us,
                                           uint32_t generation);
void mesh_sim_scheduler_cancel_role_work(struct mesh_sim_world *world,
                                         uint8_t node_index);
int mesh_sim_scheduler_next(const struct mesh_sim_world *world,
                            uint64_t end_us);
void mesh_sim_scheduler_pop(struct mesh_sim_world *world,
                            size_t event_index,
                            struct mesh_sim_event *event);

int mesh_sim_connection_process_control_packet(
    struct mesh_sim_world *world,
    uint8_t receiver_index,
    uint8_t sender_index,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool *handled);
int mesh_sim_relay_dispatch_packet(struct mesh_sim_world *world,
                                   uint8_t receiver_index,
                                   uint8_t sender_index,
                                   const struct proto_packet *packet,
                                   const uint8_t *payload,
                                   size_t payload_len);
int mesh_sim_relay_process_tick(struct mesh_sim_world *world,
                                uint8_t node_index);
int mesh_sim_relay_process_route_discovery_retry(
    struct mesh_sim_world *world,
    uint8_t node_index);
struct mesh_sim_connection_callbacks {
    void (*worker_started)(struct mesh_sim_world *world, uint8_t node_index);
    int (*worker_completed)(struct mesh_sim_world *world,
                            uint8_t node_index,
                            bool recoverable);
};

struct mesh_sim_connection_tx_result {
    bool had_packet;
    uint8_t msg_type;
};

typedef int (*mesh_sim_connection_tx_ready_fn)(
    struct mesh_sim_world *world,
    struct mesh_sim_connection_event *event,
    const struct mesh_channel5_requirements *requirements,
    const struct mesh_event_plan *plan,
    uint64_t tx_start_us,
    struct mesh_sim_connection_tx_result *result);

int mesh_sim_relay_start_connection_tx(
    struct mesh_sim_world *world,
    struct mesh_sim_connection_event *event,
    const struct mesh_channel5_requirements *requirements,
    const struct mesh_event_plan *plan,
    uint64_t tx_start_us,
    struct mesh_sim_connection_tx_result *result);

int mesh_sim_radio_schedule_channel9_runtime_rx(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t event_end_us,
    uint64_t latest_ready_us,
    uint16_t connection_event_index);
int mesh_sim_radio_schedule_channel9_runtime_tx(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t desired_air_start_us,
    uint64_t event_end_us,
    const struct mesh_outbound *outbound,
    uint16_t connection_event_index);
uint16_t mesh_sim_radio_max_propagation_for_tx(
    const struct mesh_sim_world *world,
    uint8_t node_index);

typedef int (*mesh_sim_radio_packet_handler)(
    struct mesh_sim_world *world,
    uint8_t receiver_index,
    uint8_t sender_index,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

typedef int (*mesh_sim_radio_runtime_claim_handler)(
    void *context,
    uint8_t node_index,
    enum mesh_runtime_radio_owner owner,
    uint64_t start_us,
    uint64_t end_us);

int mesh_sim_radio_evaluate_transmission(
    struct mesh_sim_world *world,
    size_t tx_index,
    mesh_sim_radio_packet_handler packet_handler,
    mesh_sim_radio_runtime_claim_handler runtime_claim_handler,
    void *runtime_claim_context);
int mesh_sim_radio_note_preamble_at_tx_start(
    struct mesh_sim_world *world,
    const struct mesh_sim_transmission *tx);
int mesh_sim_radio_process_anchor_low_duty_start(
    struct mesh_sim_world *world,
    uint8_t node_index);
int mesh_sim_radio_process_runtime_boundary(struct mesh_sim_world *world,
                                            uint8_t node_index);
enum mesh_sim_radio_state mesh_sim_radio_post_operation_state(
    const struct mesh_sim_role_instance *node,
    uint64_t now_us);

int mesh_sim_connection_process_repair_start(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    const struct mesh_sim_connection_callbacks *callbacks);
int mesh_sim_connection_process_repair_end(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    const struct mesh_sim_connection_callbacks *callbacks);
int mesh_sim_connection_process_start(
    struct mesh_sim_world *world,
    uint16_t connection_event_index,
    const struct mesh_sim_connection_callbacks *callbacks,
    mesh_sim_connection_tx_ready_fn tx_ready);
int mesh_sim_connection_process_end(
    struct mesh_sim_world *world,
    uint16_t connection_event_index);

int mesh_sim_process_event(struct mesh_sim_world *world,
                           const struct mesh_sim_event *event);

#endif
