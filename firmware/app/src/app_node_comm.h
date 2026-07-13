#ifndef APP_NODE_COMM_H
#define APP_NODE_COMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "node_comm.h"
#include "protocol.h"

#define APP_NODE_COMM_MAX_DELIVERIES 2u
#define APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN 192u

struct app_mesh_report_callbacks;
struct mesh_delivery_health;
struct mesh_outbound;
struct proto_packet;

enum app_node_comm_route_refresh_event_kind {
    APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT = 0,
    APP_NODE_COMM_ROUTE_REFRESH_BACKOFF,
    APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,
};

struct app_node_comm_route_refresh_event {
    enum app_node_comm_route_refresh_event_kind kind;
    struct proto_packet correlation;
    uint32_t gateway_sequence;
    uint8_t attempt;
    uint8_t sent_count;
    int result;
    bool correlated;
};

/*
 * Protocol-facing communication service boundary.
 *
 * The first compatibility phase deliberately aliases the proven mesh runtime
 * envelope and callbacks.  Protocol code should use this header so the backing
 * transport can be separated without another protocol-wide API migration.
 */
typedef struct mesh_outbound app_node_comm_envelope;
typedef struct app_mesh_report_callbacks app_node_comm_callbacks;
typedef struct mesh_delivery_health app_node_comm_delivery_health;

enum app_node_comm_control_send_mode {
    APP_NODE_COMM_CONTROL_WAKE_IF_NEEDED = 0,
    APP_NODE_COMM_CONTROL_ACCEPTED_EXCHANGE = 1,
};

int app_node_comm_init(const app_node_comm_callbacks *callbacks);
void app_node_comm_stop_role_scan(void);
void app_node_comm_restart_role_scan(void);
int app_node_comm_send(const app_node_comm_envelope *envelope,
                       const char *reason);
int app_node_comm_send_control(
    const app_node_comm_envelope *envelope,
    uint8_t purpose,
    enum app_node_comm_control_send_mode mode,
    const char *reason);
int app_node_comm_send_control_flood(const app_node_comm_envelope *envelope,
                                     uint8_t purpose,
                                     const char *reason,
                                     bool *sent_now);
int app_node_comm_request_path(uint64_t target_id, const char *reason);
int app_node_comm_start_delivery(const app_node_comm_envelope *envelope,
                                 const char *reason);
int app_node_comm_start_owned_delivery(const app_node_comm_envelope *envelope,
                                       const char *reason,
                                       bool *rf_sent);
int app_node_comm_queue_local_delivery(
    const app_node_comm_envelope *envelope);
int app_node_comm_submit_delivery(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_service_deliveries(void);
int app_node_comm_cancel_delivery(uint32_t handle);
bool app_node_comm_take_delivery_event(
    struct node_comm_terminal_event *event_out);
bool app_node_comm_take_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out);
size_t app_node_comm_pending_delivery_count(void);
bool app_node_comm_delivery_backlog_active(void);
bool app_node_comm_ack_wait_active(void);
void app_node_comm_delivery_health_get(app_node_comm_delivery_health *health);
bool app_node_comm_policy_running(void);
int app_node_comm_pause_request(uint32_t owner,
                                uint32_t max_hold_ms,
                                struct node_comm_pause_lease *lease_out);
int app_node_comm_pause_note_quiesced(
    const struct node_comm_pause_lease *lease);
int app_node_comm_resume_begin(const struct node_comm_pause_lease *lease);
int app_node_comm_resume_complete(const struct node_comm_pause_lease *lease);
bool app_node_comm_forced_reclaim_lease(
    struct node_comm_pause_lease *lease_out);
int app_node_comm_stop_preserving_queued(void);
int app_node_comm_start(void);
void app_node_comm_lifecycle_service(void);
void app_node_comm_start_route_refresh(void);
int app_node_comm_request_route_refresh(uint32_t delay_ms,
                                        const char *reason,
                                        bool forced);
int app_node_comm_request_route_refresh_correlated(
    uint32_t delay_ms,
    const char *reason,
    const struct proto_packet *correlation);

#endif
