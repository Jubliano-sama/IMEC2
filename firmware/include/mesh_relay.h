#ifndef MESH_RELAY_H
#define MESH_RELAY_H

#include "mesh.h"
#include "protocol.h"
#include "route.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_BROADCAST_ID 0u
#define MESH_RELAY_DOWNLINK_ROUTES 16u
#define MESH_RELAY_DUP_CACHE_SIZE 16u
#define MESH_RELAY_EVENT_TIMINGS 16u
#define MESH_RELAY_DOWNLINK_MAX_FAILURES 3u

enum mesh_relay_role {
    MESH_RELAY_ROLE_ANCHOR = 1,
    MESH_RELAY_ROLE_GATEWAY = 2,
};

enum mesh_relay_action {
    MESH_RELAY_ACTION_NONE = 0u,
    MESH_RELAY_ACTION_DELIVER_LOCAL = 1u << 1,
    MESH_RELAY_ACTION_FORWARD = 1u << 2,
    MESH_RELAY_ACTION_SEND_GATEWAY_ACK = 1u << 3,
    MESH_RELAY_ACTION_DROP = 1u << 6,
    MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED = 1u << 8,
    MESH_RELAY_ACTION_RETRANSMIT = 1u << 9,
    MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED = 1u << 10,
    MESH_RELAY_ACTION_SEND_ROUTE_REQ = 1u << 11,
    MESH_RELAY_ACTION_SEND_ROUTE_REPLY = 1u << 12,
    MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY = 1u << 13,
};

enum mesh_relay_tx_state {
    MESH_RELAY_TX_IDLE = 0,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK = 1,
};

struct mesh_outbound {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
};

struct mesh_downlink_entry {
    uint64_t target_id;
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;
    uint32_t last_seen_ms;
    uint8_t hop_count;
    uint8_t quality;
    bool valid;
};

struct mesh_duplicate_entry {
    uint8_t msg_type;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint32_t last_seen_ms;
    uint16_t seq;
    bool valid;
};

struct mesh_relay_event_timing_entry {
    uint64_t next_hop_id;
    struct mesh_event_timing timing;
    bool valid;
};

struct mesh_pending_tx {
    enum mesh_relay_tx_state state;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t gateway_ack_deadline_ms;
};

struct mesh_relay {
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    struct route_table upstream;
    struct mesh_downlink_entry downlinks[MESH_RELAY_DOWNLINK_ROUTES];
    struct mesh_duplicate_entry duplicates[MESH_RELAY_DUP_CACHE_SIZE];
    struct mesh_relay_event_timing_entry event_timings[MESH_RELAY_EVENT_TIMINGS];
    struct mesh_pending_tx pending;
    uint8_t duplicate_next;
    uint16_t next_seq;
};

struct mesh_relay_result {
    uint32_t actions;
    int status;
    struct mesh_outbound forward;
    struct mesh_outbound gateway_ack;
    struct mesh_outbound route_request;
    struct mesh_outbound route_reply;
    struct mesh_outbound retransmit;
};

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch);
const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id);
int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id);
int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing);
void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id);
int mesh_relay_require_channel9_event(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      const struct mesh_channel5_requirements *requirements,
                                      uint32_t now_ms,
                                      struct mesh_event_plan *plan);
uint8_t mesh_relay_expire_channel9_timings(struct mesh_relay *relay,
                                           uint32_t now_ms);
uint8_t mesh_relay_expire_routes(struct mesh_relay *relay, uint32_t now_ms);
int mesh_relay_build_route_request(struct mesh_relay *relay,
                                   uint64_t target_id,
                                   struct mesh_outbound *out,
                                   uint32_t now_ms);
int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset);
bool mesh_relay_tx_active(const struct mesh_relay *relay);
void mesh_relay_cancel_tx(struct mesh_relay *relay);
int mesh_relay_start_tx(struct mesh_relay *relay,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        struct mesh_outbound *out);
int mesh_relay_start_channel9_tx(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const struct mesh_channel5_requirements *requirements,
                                 uint32_t now_ms,
                                 struct mesh_event_plan *plan,
                                 struct mesh_outbound *out);
void mesh_relay_note_channel9_success(struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t event_start_ms);
void mesh_relay_note_channel9_rx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t planned_event_start_ms,
                                 uint32_t observed_packet_ms);
void mesh_relay_note_channel9_missed(struct mesh_relay *relay,
                                     uint64_t next_hop_id,
                                     struct mesh_event_diagnostics *diagnostics);
void mesh_relay_note_tx_sent(struct mesh_relay *relay,
                             const struct mesh_outbound *out,
                             uint32_t now_ms);
void mesh_relay_note_delivery_failure(struct mesh_relay *relay,
                                      uint64_t dst_id);
int mesh_relay_tick(struct mesh_relay *relay,
                    uint32_t now_ms,
                    struct mesh_relay_result *result);
int mesh_relay_handle_rx(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t previous_hop_id,
                         uint8_t link_quality,
                         uint32_t now_ms,
                         struct mesh_relay_result *result);

#ifdef __cplusplus
}
#endif

#endif
