#ifndef APP_MESH_REPORT_H
#define APP_MESH_REPORT_H

#include "dwm3000_driver.h"
#include "app_mesh_route_wait_tx.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "uwb_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct k_work_delayable;
struct app_mesh_command_orchestrator;
struct app_node_comm_route_refresh_event;

struct uwb_range_schedule_frame;

struct mesh_delivery_health {
    uint32_t ack_retry_admission_failures;
    uint32_t oldest_ack_pending_age_ms;
    uint32_t permanent_report_failures;
    uint32_t last_permanent_error;
    uint32_t last_permanent_session_id;
    uint16_t last_permanent_seq;
    uint8_t last_permanent_msg_type;
};

struct anchor_range_window_report {
    struct dwm3000_range_result result;
    int32_t distance_samples_mm[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t sample_sequence_start_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    uint8_t range_round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t distance_sum_mm;
    int64_t first_exchange_start_ms;
    uint32_t quality_sum;
    uint16_t sample_count;
    bool have_result;
    bool have_exchange_start_ms;
    bool rsl_sampled;
    bool cir_sampled;
    bool anchor_full_cir_sampled;
};

struct app_mesh_report_callbacks {
    bool (*anchor_survey_discovery_is_pending)(void);
    void (*anchor_note_uwb_awake_since)(int64_t start_ms,
                                        uint32_t already_counted_us);
    bool (*anchor_handle_click_wake_claim)(
        const struct uwb_wake_claim_frame *claim,
        uint8_t link_quality,
        int64_t received_at_ms);
    void (*anchor_handle_local_command)(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len);
    void (*anchor_handle_survey_discovery_start)(const struct proto_packet *packet,
                                                 const uint8_t *payload,
                                                 size_t payload_len);
    void (*anchor_handle_survey_pair_prepare)(const struct proto_packet *packet,
                                              const uint8_t *payload,
                                              size_t payload_len);
    void (*gateway_handle_survey_discovery_report)(const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len,
                                                   uint64_t previous_hop_id,
                                                   uint8_t radio_channel,
                                                   uint8_t link_quality);
    void (*anchor_survey_delivery_gateway_confirmed)(const struct proto_packet *packet);
    void (*anchor_survey_delivery_transport_released)(
        const struct proto_packet *packet,
        bool preempted);
    void (*gateway_route_refresh_event)(
        const struct app_node_comm_route_refresh_event *event);
};

struct app_mesh_outbound_view {
    const struct proto_packet *packet;
    const uint8_t *payload;
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
    uint8_t flood_retry_count;
};

enum mesh_c5_control_send_mode {
    MESH_C5_CONTROL_WAKE_IF_NEEDED = 0,
    MESH_C5_CONTROL_ACCEPTED_EXCHANGE = 1,
};

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks);
int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     int64_t local_ms);
int append_range_result_timing_tlvs(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    const struct dwm3000_range_result *result);
int append_anchor_status_tlvs(uint8_t *payload,
                              size_t payload_cap,
                              size_t *payload_len);
void build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report);
uint8_t *mesh_anchor_click_cir_capture_begin(size_t *capacity);
void mesh_stop_role_scan(void);
void mesh_restart_role_scan(void);
int mesh_transport_pause_preserving_queued(void);
bool mesh_transport_quiesced(void);
void mesh_transport_resume(void);
int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason);
int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason,
                       bool *sent_now);
int mesh_try_send_c5_flood(const struct mesh_outbound *out,
                           uint8_t purpose,
                           const char *reason,
                           bool *sent_now);
int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool *sent_now);
int mesh_send_gateway_command_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now);
struct app_mesh_command_orchestrator *mesh_gateway_command_orchestrator_context(void);
void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
int mesh_prepare_channel9_outbound(struct mesh_outbound *out,
                                   const struct mesh_event_plan *plan,
                                   uint32_t now_ms,
                                   uint32_t *required_ms);
int mesh_request_route(uint64_t target_id, const char *reason);
void mesh_clear_route_waiting_tx(const struct proto_packet *packet);
int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
int mesh_start_owned_tracked_tx(const struct mesh_outbound *out,
                                const char *reason,
                                bool *rf_sent);
int mesh_owned_tracked_tx_preflight(const struct mesh_outbound *out,
                                    const char *reason,
                                    enum app_mesh_route_wait_tx_owner owner,
                                    uint32_t generation);
void mesh_report_resume_restored_outbox(const char *reason);
int mesh_preempt_for_click_event(void);
void report_tx_schedule(uint32_t delay_ms);
uint32_t report_tx_queue_used(void);
bool mesh_report_tx_backlog_active(void);
bool mesh_report_ch9_ack_wait_active(void);
int queue_anchor_report(const struct mesh_outbound *outbound);
bool mesh_queue_from_frame(const uint8_t *frame,
                           size_t frame_len,
                           uint8_t link_quality,
                           uint8_t radio_channel,
                           bool *valid_mesh_frame,
                           uint64_t *previous_hop_id);
bool mesh_queue_from_frame_deferred(const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t link_quality,
                                    uint8_t radio_channel,
                                    bool *valid_mesh_frame,
                                    uint64_t *previous_hop_id);
bool mesh_anchor_handoff_route_wake_frame(const uint8_t *frame,
                                          size_t frame_len,
                                          uint8_t link_quality);
void mesh_submit_queued_rx(void);
bool mesh_process_queued_rx_now(const char *reason);
int mesh_start_uwb_rx(const char *reason);
void mesh_delivery_health_get(struct mesh_delivery_health *health);
bool mesh_route_waiting_tx_active(void);
uint32_t mesh_rx_pending_count(void);
bool mesh_rx_response_active(void);
bool mesh_anchor_low_duty_scan_should_defer(uint32_t *retry_ms);
bool mesh_anchor_connected_radio_active(void);
int mesh_gateway_command_priority_submit(struct k_work_delayable *work);
int mesh_route_work_reschedule(struct k_work_delayable *work, uint32_t delay_ms);

#endif
