#ifndef MESH_SIM_H
#define MESH_SIM_H

#include "mesh.h"
#include "mesh_relay.h"
#include "mesh_radio_timing.h"
#include "mesh_runtime.h"
#include "protocol.h"
#include "dwm3000_runtime.h"
#include "uwb_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_sim_world;

#define MESH_SIM_MAX_ROLES 64u
#define MESH_SIM_MAX_CONNECTIONS 24u
#define MESH_SIM_MAX_CONNECTION_EVENTS 1024u
#define MESH_SIM_MAX_EVENTS 4096u
#define MESH_SIM_MAX_RX_WINDOWS 512u
#define MESH_SIM_MAX_TRANSMISSIONS 512u
#define MESH_SIM_MAX_RECEPTIONS 1024u
#define MESH_SIM_MAX_TRANSITIONS 2048u
#define MESH_SIM_TX_QUEUE_CAPACITY 16u
#define MESH_SIM_DELIVERY_CAPACITY 256u
#define MESH_SIM_SLOT_TX_OFFSET_US 100u
#define MESH_SIM_WATCHDOG_PRODUCTION_TIMEOUT_US UINT64_C(180000000)
#define MESH_SIM_WATCHDOG_PRODUCTION_LEASE_US UINT64_C(120000000)

enum mesh_sim_status {
    MESH_SIM_OK = 0,
    MESH_SIM_ERR_ARG = -1000,
    MESH_SIM_ERR_CAPACITY = -1001,
    MESH_SIM_ERR_RADIO_CONFLICT = -1002,
    MESH_SIM_ERR_FRAME_TOO_LONG = -1003,
    MESH_SIM_ERR_EVENT_ORDER = -1004,
    MESH_SIM_ERR_ROUTE_REQUIRED = -1005,
    MESH_SIM_ERR_UNSUPPORTED_ACTION = -1006,
    MESH_SIM_ERR_PROTOCOL = -1007,
    MESH_SIM_ERR_WATCHDOG = -1008,
    MESH_SIM_ERR_SLOT_DIRECTION = -1009,
    MESH_SIM_ERR_CONNECTION_PLAN = -1010,
    MESH_SIM_ERR_SENDER_PLAN = -1011,
};

enum mesh_sim_watchdog_action {
    MESH_SIM_WATCHDOG_FAIL = 0,
    MESH_SIM_WATCHDOG_RESET_ROLE = 1,
};

enum mesh_sim_role {
    MESH_SIM_ROLE_CLICKER = 0,
    MESH_SIM_ROLE_TRANSMITTER = 1,
    MESH_SIM_ROLE_ANCHOR = 2,
    MESH_SIM_ROLE_GATEWAY = 3,
};

enum mesh_sim_radio_state {
    MESH_SIM_RADIO_SLEEP = 0,
    MESH_SIM_RADIO_RX = 1,
    MESH_SIM_RADIO_TX = 2,
    MESH_SIM_RADIO_IDLE = 3,
};

enum mesh_sim_phy {
    MESH_SIM_PHY_CHANNEL5_WAKE = 0,
    MESH_SIM_PHY_CHANNEL5_RANGE = 1,
    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL = 2,
    MESH_SIM_PHY_CHANNEL9_MESH = 3,
};

enum mesh_sim_connection_action_kind {
    MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT = 0,
    MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR = 1,
    MESH_SIM_CONNECTION_ACTION_NONE = 2,
};

enum mesh_sim_rx_outcome {
    MESH_SIM_RX_DECODED = 0,
    MESH_SIM_RX_PREAMBLE_ONLY = 1,
    MESH_SIM_RX_SFD_TIMEOUT = 2,
    MESH_SIM_RX_FRAME_TIMEOUT = 3,
    MESH_SIM_RX_COLLISION = 4,
    MESH_SIM_RX_DECODE_ERROR = 5,
};

enum mesh_sim_transition_kind {
    MESH_SIM_TRANSITION_RX_START = 0,
    MESH_SIM_TRANSITION_RX_END = 1,
    MESH_SIM_TRANSITION_TX_START = 2,
    MESH_SIM_TRANSITION_TX_END = 3,
    MESH_SIM_TRANSITION_RX_DECODED = 4,
    MESH_SIM_TRANSITION_RX_PARTIAL = 5,
    MESH_SIM_TRANSITION_RX_COLLISION = 6,
    MESH_SIM_TRANSITION_PACKET_QUEUED = 7,
    MESH_SIM_TRANSITION_PACKET_DELIVERED = 8,
    MESH_SIM_TRANSITION_GATEWAY_ACKED = 9,
    MESH_SIM_TRANSITION_HOP_PROGRESS = 10,
    MESH_SIM_TRANSITION_RETRY_READY = 11,
    MESH_SIM_TRANSITION_ROUTE_REQUIRED = 12,
    MESH_SIM_TRANSITION_CONNECTION_EVENT = 13,
    MESH_SIM_TRANSITION_CONNECTION_PREEMPTED = 14,
    MESH_SIM_TRANSITION_RUNTIME_GATEWAY_COMMAND = 15,
    MESH_SIM_TRANSITION_RUNTIME_LOCAL_CLICK = 16,
    MESH_SIM_TRANSITION_RUNTIME_EVENT_REPAIR = 17,
    MESH_SIM_TRANSITION_RUNTIME_TRANSIT = 18,
    MESH_SIM_TRANSITION_RADIO_RECONFIGURED = 19,
    MESH_SIM_TRANSITION_RX_WINDOW_EXTENDED = 20,
    MESH_SIM_TRANSITION_LOW_DUTY_RESCHEDULED = 21,
    MESH_SIM_TRANSITION_RUNTIME_RADIO_RELEASED = 22,
    MESH_SIM_TRANSITION_WATCHDOG_ARMED = 23,
    MESH_SIM_TRANSITION_WATCHDOG_FED = 24,
    MESH_SIM_TRANSITION_WATCHDOG_EXPIRED = 25,
    MESH_SIM_TRANSITION_WATCHDOG_RESET = 26,
    MESH_SIM_TRANSITION_WAKE_CLAIM_OWNED = 27,
    MESH_SIM_TRANSITION_DS_TWR_RELEASED = 28,
    MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED = 29,
    MESH_SIM_TRANSITION_CONNECTION_REPAIRED = 30,
    MESH_SIM_TRANSITION_CONNECTION_EVENTS_SKIPPED = 31,
};

struct mesh_sim_connection_action {
    uint64_t start_us;
    uint64_t end_us;
    enum mesh_sim_connection_action_kind kind;
    uint8_t skipped_events;
    bool already_scheduled;
};

struct mesh_sim_watchdog {
    uint64_t timeout_us;
    uint64_t last_feed_us;
    uint64_t deadline_us;
    uint32_t feeds;
    uint32_t expirations;
    uint32_t resets;
    enum mesh_sim_watchdog_action action;
    enum mesh_runtime_radio_owner expired_radio_owner;
    enum mesh_sim_radio_state expired_radio_state;
    enum mesh_relay_tx_state expired_pending_state;
    uint16_t expired_queue_count;
    bool armed;
    bool expired;
};

struct mesh_sim_phy_profile {
    uint32_t preamble_us;
    uint16_t sfd_us;
    uint16_t phr_us;
    uint32_t bitrate_bps;
};

struct mesh_sim_transition {
    uint64_t time_us;
    uint64_t node_id;
    uint64_t peer_id;
    enum mesh_sim_transition_kind kind;
    uint32_t detail;
    uint8_t msg_type;
};

struct mesh_sim_reception {
    uint64_t start_us;
    uint64_t end_us;
    uint64_t start_rctu;
    uint64_t end_rctu;
    uint64_t source_id;
    uint64_t receiver_id;
    enum mesh_sim_rx_outcome outcome;
    enum mesh_sim_phy phy;
    uint8_t channel;
    int protocol_status;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
};

struct mesh_sim_delivery {
    uint64_t delivered_at_us;
    uint64_t previous_hop_id;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
};

struct mesh_sim_queued_tx {
    struct mesh_outbound outbound;
    uint32_t enqueue_order;
    uint8_t priority;
    bool needs_relay_start;
    bool valid;
};

struct mesh_sim_role_instance {
    enum mesh_sim_role role;
    enum mesh_sim_radio_state radio_state;
    uint64_t id;
    uint64_t gateway_id;
    struct mesh_relay relay;
    struct mesh_runtime runtime;
    struct dwm3000_runtime dwm3000;
    struct mesh_sim_world *world;
    struct uwb_clicker_session clicker_session;
    struct uwb_anchor_session anchor_session;
    struct mesh_sim_watchdog watchdog;
    struct mesh_sim_queued_tx tx_queue[MESH_SIM_TX_QUEUE_CAPACITY];
    struct mesh_sim_delivery deliveries[MESH_SIM_DELIVERY_CAPACITY];
    size_t tx_queue_count;
    size_t delivery_count;
    uint32_t route_discovery_requests;
    uint32_t decoded_frames;
    uint32_t partial_frames;
    uint32_t collision_frames;
    uint32_t runtime_action_duration_us[4];
    uint32_t next_relay_random;
    uint8_t node_index;
    bool relay_initialized;
    bool clicker_initialized;
    bool anchor_initialized;
    bool resume_low_duty_after_ds_twr;
    bool next_relay_random_valid;
};

struct mesh_sim_connection {
    uint8_t node_a;
    uint8_t node_b;
    struct mesh_event_timing timing_a;
    struct mesh_event_timing timing_b;
    struct mesh_event_diagnostics diagnostics_a;
    struct mesh_event_diagnostics diagnostics_b;
    uint64_t repair_start_us;
    uint64_t repair_end_us;
    uint32_t completed_events;
    uint32_t completed_repairs;
    uint16_t repair_control_frame_len;
    uint8_t repair_requester;
    bool repair_pending;
    bool valid;
};

struct mesh_sim_event {
    uint64_t time_us;
    uint32_t sequence;
    uint16_t object_index;
    uint8_t type;
    uint8_t priority;
    bool pending;
};

struct mesh_sim_rx_window {
    uint64_t start_us;
    uint64_t end_us;
    uint64_t start_rctu;
    uint64_t end_rctu;
    uint64_t initial_end_rctu;
    uint32_t activity_completion_us;
    uint8_t node_index;
    uint8_t channel;
    enum mesh_sim_phy phy;
    bool preamble_detected;
    bool extend_on_activity;
    bool continuous_operation;
    bool periodic_low_duty;
    bool wake_claim_handoff;
    bool valid;
};

struct mesh_sim_transmission {
    uint64_t start_us;
    uint64_t end_us;
    uint64_t start_rctu;
    uint64_t rmarker_rctu;
    uint64_t end_rctu;
    uint8_t node_index;
    uint8_t channel;
    enum mesh_sim_phy phy;
    uint16_t frame_len;
    uint16_t connection_event_index;
    bool protocol_frame;
    bool has_outbound;
    bool valid;
    uint8_t frame[PACKET_EXT_MAX_LEN];
    struct mesh_outbound outbound;
};

struct mesh_sim_connection_event {
    uint64_t start_us;
    uint64_t end_us;
    uint16_t connection_index;
    uint8_t sender_index;
    uint8_t receiver_index;
    bool receiver_preempted;
    bool had_packet;
    bool decoded;
    bool valid;
};

struct mesh_sim_world {
    uint64_t now_us;
    uint32_t rng_state;
    uint32_t next_sequence;
    uint32_t next_enqueue_order;
    int last_error;
    struct mesh_sim_role_instance roles[MESH_SIM_MAX_ROLES];
    struct mesh_sim_connection connections[MESH_SIM_MAX_CONNECTIONS];
    struct mesh_sim_connection_event connection_events[MESH_SIM_MAX_CONNECTION_EVENTS];
    struct mesh_sim_event events[MESH_SIM_MAX_EVENTS];
    struct mesh_sim_rx_window rx_windows[MESH_SIM_MAX_RX_WINDOWS];
    struct mesh_sim_transmission transmissions[MESH_SIM_MAX_TRANSMISSIONS];
    struct mesh_sim_reception receptions[MESH_SIM_MAX_RECEPTIONS];
    struct mesh_sim_transition transitions[MESH_SIM_MAX_TRANSITIONS];
    bool reachable[MESH_SIM_MAX_ROLES][MESH_SIM_MAX_ROLES];
    uint8_t link_quality[MESH_SIM_MAX_ROLES][MESH_SIM_MAX_ROLES];
    uint16_t propagation_us[MESH_SIM_MAX_ROLES][MESH_SIM_MAX_ROLES];
    uint64_t propagation_rctu[MESH_SIM_MAX_ROLES][MESH_SIM_MAX_ROLES];
    size_t role_count;
    size_t connection_count;
    size_t connection_event_count;
    size_t event_count;
    size_t rx_window_count;
    size_t transmission_count;
    size_t reception_count;
    size_t transition_count;
};

void mesh_sim_init(struct mesh_sim_world *world, uint32_t seed);
uint32_t mesh_sim_random(struct mesh_sim_world *world);
const struct mesh_sim_phy_profile *mesh_sim_phy_profile(enum mesh_sim_phy phy);
uint32_t mesh_sim_frame_duration_us(enum mesh_sim_phy phy, size_t frame_len);

int mesh_sim_add_role(struct mesh_sim_world *world,
                      enum mesh_sim_role role,
                      uint64_t id,
                      uint64_t gateway_id,
                      uint32_t route_epoch,
                      uint8_t *node_index);
struct mesh_sim_role_instance *mesh_sim_role(struct mesh_sim_world *world,
                                             uint8_t node_index);
int mesh_sim_init_clicker_session(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  const struct uwb_clicker_config *config);
int mesh_sim_init_anchor_session(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 const struct uwb_anchor_config *config);

int mesh_sim_set_link(struct mesh_sim_world *world,
                      uint8_t node_a,
                      uint8_t node_b,
                      uint8_t quality,
                      uint16_t propagation_us);
int mesh_sim_install_route(struct mesh_sim_world *world,
                           uint8_t node_index,
                           uint8_t next_hop_index,
                           uint8_t hop_count,
                           uint32_t route_epoch);
int mesh_sim_install_downlink(struct mesh_sim_world *world,
                              uint8_t node_index,
                              uint64_t target_id,
                              uint8_t next_hop_index,
                              uint8_t hop_count,
                              uint32_t route_epoch);

int mesh_sim_add_connection(struct mesh_sim_world *world,
                            uint8_t node_a,
                            uint8_t node_b,
                            const struct mesh_event_params *params,
                            bool node_a_transmits_first,
                            uint16_t *connection_index);
int mesh_sim_schedule_next_connection_event(struct mesh_sim_world *world,
                                            uint16_t connection_index,
                                            bool receiver_preempted);
int mesh_sim_connection_next_action(const struct mesh_sim_world *world,
                                    uint16_t connection_index,
                                    struct mesh_sim_connection_action *action);
uint32_t mesh_sim_connection_next_event_ms(const struct mesh_sim_world *world,
                                           uint16_t connection_index);

int mesh_sim_queue_originated(struct mesh_sim_world *world,
                              uint8_t node_index,
                              const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len);
int mesh_sim_schedule_relay_tick(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t at_us);

int mesh_sim_schedule_rx(struct mesh_sim_world *world,
                         uint8_t node_index,
                         uint64_t start_us,
                         uint64_t end_us,
                         uint8_t channel,
                         enum mesh_sim_phy phy,
                         uint16_t *window_index);
int mesh_sim_schedule_rx_extend_on_activity(struct mesh_sim_world *world,
                                            uint8_t node_index,
                                            uint64_t start_us,
                                            uint64_t acquisition_end_us,
                                            uint32_t completion_us,
                                            uint8_t channel,
                                            enum mesh_sim_phy phy,
                                            uint16_t *window_index);
int mesh_sim_start_anchor_low_duty(struct mesh_sim_world *world,
                                   uint8_t node_index,
                                   uint64_t work_start_us);
int mesh_sim_runtime_submit(struct mesh_sim_world *world,
                            uint8_t node_index,
                            enum mesh_runtime_work_kind kind,
                            uint64_t token,
                            uint64_t ready_us);
int mesh_sim_runtime_reserve_transit(struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     const struct mesh_outbound *outbound,
                                     uint64_t ready_us);
int mesh_sim_runtime_claim_radio(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 enum mesh_runtime_radio_owner owner,
                                 uint64_t start_us,
                                 uint64_t end_us);
int mesh_sim_runtime_set_action_duration(
    struct mesh_sim_world *world,
    uint8_t node_index,
    enum mesh_runtime_work_kind kind,
    uint32_t duration_us);
int mesh_sim_watchdog_arm(struct mesh_sim_world *world,
                          uint8_t node_index,
                          uint64_t timeout_us,
                          enum mesh_sim_watchdog_action action);
int mesh_sim_watchdog_feed(struct mesh_sim_world *world,
                           uint8_t node_index);
int mesh_sim_schedule_raw_tx(struct mesh_sim_world *world,
                             uint8_t node_index,
                             uint64_t start_us,
                             uint8_t channel,
                             enum mesh_sim_phy phy,
                             const uint8_t *frame,
                             size_t frame_len,
                             bool protocol_frame,
                             uint16_t *transmission_index);
int mesh_sim_schedule_packet_tx(struct mesh_sim_world *world,
                                uint8_t node_index,
                                uint64_t start_us,
                                uint8_t channel,
                                enum mesh_sim_phy phy,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *transmission_index);
int mesh_sim_outbound_radio(const struct mesh_outbound *outbound,
                            uint8_t *channel,
                            enum mesh_sim_phy *phy);
int mesh_sim_schedule_outbound_tx(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  uint64_t start_us,
                                  const struct mesh_outbound *outbound,
                                  uint16_t *transmission_index);
int mesh_sim_override_next_relay_random(struct mesh_sim_world *world,
                                        uint8_t node_index,
                                        uint32_t random_value);

int mesh_sim_run_until(struct mesh_sim_world *world, uint64_t end_us);
int mesh_sim_run(struct mesh_sim_world *world);

size_t mesh_sim_count_transitions(const struct mesh_sim_world *world,
                                  enum mesh_sim_transition_kind kind,
                                  uint64_t node_id);
const struct mesh_sim_transition *mesh_sim_find_transition(
    const struct mesh_sim_world *world,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    size_t occurrence);

#ifdef __cplusplus
}
#endif

#endif
